/*-------------------------------------------------------------------------
 *
 * auto_explain_z_dump.c
 *	  Decode auto_explain_z binary logs.
 *
 *-------------------------------------------------------------------------
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pg_config.h"

#ifdef USE_LZ4
#include <lz4.h>
#include <lz4frame.h>
#endif
#ifdef USE_ZSTD
#include <zstd.h>
#endif

#define FILE_MAGIC					UINT32_C(0x315a4541)
#define RECORD_MAGIC					UINT32_C(0x325a4541)
#define PAYLOAD_MAGIC				UINT32_C(0x335a4541)
#define STRING_NULL					UINT32_C(0xffffffff)
#define STRING_NULL_CTRL				0x80
#define CONTEXT_REF					0x80

#define COMPRESSION_NONE				0
#define COMPRESSION_LZ4				1
#define COMPRESSION_ZSTD				2

#define PROFILE_SIMPLE				0
#define PROFILE_FULL					1

#define TEMPLATE_NONE				0
#define TEMPLATE_DEFINE				1
#define TEMPLATE_REF					2
#define TEMPLATE_HAS_METRICS			0x40
#define TEMPLATE_HAS_DETAILS			0x80

#define FLAG_ANALYZE					0x00000001
#define FLAG_VERBOSE					0x00000002
#define FLAG_COSTS					0x00000004
#define FLAG_TIMING					0x00000008
#define FLAG_BUFFERS					0x00000010
#define FLAG_WAL						0x00000020
#define FLAG_QUERY_TEXT				0x00000040
#define FLAG_PARAMS					0x00000080

#define DEFAULT_LOG_LINE_PREFIX		"%m [%p] "
#define DEFAULT_LOG_TIMEZONE			"GMT"
#define DEFAULT_LOG_ERROR_VERBOSITY	"default"

#define NODE_PARALLEL_AWARE			0x0001
#define NODE_ASYNC_CAPABLE			0x0002
#define NODE_HAS_ACTUAL				0x0004
#define NODE_NEVER_EXECUTED			0x0008
#define NODE_HAS_BUFFERS				0x0010
#define NODE_HAS_WAL					0x0020
#define NODE_DISABLED				0x0040

#define EXTRA_MODIFY_OPERATION		1
#define EXTRA_FOREIGN_OPERATION		2
#define EXTRA_JOIN_TYPE				3
#define EXTRA_AGG					4
#define EXTRA_SETOP					5
#define EXTRA_INDEX_SCAN_DIRECTION	6

#define OBJ_RELATION					0x01
#define OBJ_INDEX					0x02

#define DETAIL_STRING				1
#define DETAIL_STRING_LIST			2
#define DETAIL_DOUBLE				3
#define DETAIL_INT64					4
#define DETAIL_UINT64				5
#define DETAIL_BOOL					6

typedef struct Buf
{
	char	   *data;
	size_t		len;
	size_t		cap;
} Buf;

typedef enum ValueType
{
	V_NULL,
	V_BOOL,
	V_INT,
	V_UINT,
	V_DOUBLE,
	V_NUMERIC,
	V_STRING,
	V_ARRAY,
	V_OBJECT,
} ValueType;

typedef struct Value Value;

typedef struct Array
{
	Value	  **items;
	size_t		len;
	size_t		cap;
} Array;

typedef struct Pair
{
	char	   *key;
	Value	   *value;
} Pair;

typedef struct Object
{
	Pair	   *pairs;
	size_t		len;
	size_t		cap;
} Object;

struct Value
{
	ValueType	type;
	union
	{
		bool		b;
		int64_t		i;
		uint64_t	u;
		double		d;
		char	   *s;
		Array	   *a;
		Object	   *o;
	} v;
};

typedef struct Reader
{
	const uint8_t *data;
	size_t		len;
	size_t		pos;
	int			version;
} Reader;

typedef struct Binary
{
	uint8_t    *data;
	size_t		len;
} Binary;

typedef struct TemplateEntry
{
	uint64_t	id;
	uint64_t	query_id;
	uint64_t	shape_hash;
	Value	   *plan;
} TemplateEntry;

typedef struct TemplateStore
{
	TemplateEntry *entries;
	size_t		len;
	size_t		cap;
	Value	   *context;
} TemplateStore;

typedef enum OutputFormat
{
	FMT_TEXT,
	FMT_JSON,
	FMT_YAML,
	FMT_XML,
} OutputFormat;

typedef struct Options
{
	OutputFormat format;
	bool		raw;
	bool		verify_crc;
	bool		postgres_log;
	const char *log_line_prefix;
	const char *log_timezone;
	const char *log_error_verbosity;
	int			first_file_arg;
} Options;

typedef struct ParsedTimestamp
{
	time_t		sec;
	int			usec;
	bool		valid;
} ParsedTimestamp;

typedef struct ServerLogConfig
{
	const char *log_line_prefix;
	const char *log_timezone;
	bool		verbose;
} ServerLogConfig;

static bool verify_crc = false;

static void *xmalloc(size_t size);
static void *xrealloc(void *ptr, size_t size);
static char *xstrdup(const char *s);
static char *xstrndup(const char *s, size_t len);
static char *xasprintf(const char *fmt,...) __attribute__((format(printf, 1, 2)));
static void fatal(const char *fmt,...) __attribute__((format(printf, 1, 2), noreturn));

static Value *value_new(ValueType type);
static Value *value_null(void);
static Value *value_bool(bool b);
static Value *value_int(int64_t i);
static Value *value_uint(uint64_t u);
static Value *value_double(double d);
static Value *value_numeric(const char *s);
static Value *value_string(const char *s);
static Value *value_string_steal(char *s);
static Value *value_array(void);
static Value *value_object(void);
static Value *value_copy(const Value *value);

static void array_append(Value *array, Value *value);
static size_t array_len(const Value *array);
static Value *array_get(const Value *array, size_t idx);

static void object_set(Value *object, const char *key, Value *value);
static void object_set_append(Value *object, const char *key, Value *value);
static Value *object_get(const Value *object, const char *key);
static Value *object_remove(Value *object, const char *key);
static bool object_has(const Value *object, const char *key);

static int64_t value_i64(const Value *value, int64_t def);
static uint64_t value_u64(const Value *value, uint64_t def);
static double value_double_as(const Value *value, double def);
static const char *value_cstr(const Value *value);
static bool value_truthy(const Value *value);

static void buf_init(Buf *buf);
static void buf_reserve(Buf *buf, size_t needed);
static void buf_append(Buf *buf, const char *data, size_t len);
static void buf_appendc(Buf *buf, char c);
static void buf_appends(Buf *buf, const char *s);
static void buf_appendf(Buf *buf, const char *fmt,...) __attribute__((format(printf, 2, 3)));
static char *buf_steal(Buf *buf);

static uint8_t reader_u8(Reader *r);
static uint16_t reader_u16(Reader *r);
static uint32_t reader_u32(Reader *r);
static int32_t reader_i32(Reader *r);
static uint64_t reader_u64(Reader *r);
static int64_t reader_i64(Reader *r);
static double reader_double(Reader *r);
static uint64_t reader_uint_sized(Reader *r, unsigned code);
static int64_t reader_int_sized(Reader *r, unsigned code);
static char *reader_string(Reader *r);
static Binary reader_take(Reader *r, size_t len);

static Binary read_file(const char *path);
static Binary decompress_file_body(uint16_t method, const uint8_t *data, size_t len);
static Binary decompress_payload(uint16_t method, const uint8_t *data, size_t len,
								 size_t expected_len);
static uint32_t crc32c_sw(const uint8_t *data, size_t len);

static const char *compression_name(uint16_t method);
static const char *profile_name(unsigned profile);
static const char *template_mode_name(unsigned mode);
static const char *relationship_name(unsigned rel);
static const char *detail_label(unsigned code);
static const char *node_name(unsigned tag);
static bool detail_is_dynamic(unsigned code);

static char *pg_timestamp_to_iso(int64_t ts);
static bool parse_iso_timestamp(const char *s, ParsedTimestamp *out);
static char *format_log_timestamp_ms(const char *s);
static char *format_log_timestamp(const char *s);
static char *format_log_epoch_ms(const char *s);
static char *format_session_id(const Value *ctx);
static ServerLogConfig log_config_from_options(const Options *opts);
static char *numeric_fixed(double value, int digits);
static int64_t pg_signed_i64(uint64_t value);

static Value *parse_details(Reader *r);
static Value *parse_actual(Reader *r, uint16_t flags);
static Value *parse_buffers(Reader *r);
static Value *parse_wal(Reader *r);
static void parse_identity(Reader *r, Value *node);
static Value *parse_full_plan_node(Reader *r);
static Value *parse_metric_node(Reader *r);
static Value *strip_dynamic_details(const Value *node);
static Value *merge_template_metrics(const Value *template_plan, const Value *metrics);
static Value *detail_to_property(const Value *detail);
static Value *apply_details(Value *node);
static Value *parse_payload(const uint8_t *data, size_t len, Value *record_header,
							TemplateStore *templates);
static void parse_file_records(const char *path, Value *records);

static Value *postgres_records(const Value *records);
static Value *postgres_record(const Value *record);
static Value *postgres_plan_node(const Value *node, uint32_t record_flags);

static void render_json_value(Buf *buf, const Value *value, int indent, const char *key);
static void render_yaml_obj(Buf *buf, const Value *value, int indent);
static void render_xml_value(Buf *buf, const char *key, const Value *value, int indent);
static void render_xml_document(Buf *buf, const Value *records);
static void render_postgres_text(Buf *buf, const Value *records);
static void render_postgres_log(Buf *buf, const Value *records,
								OutputFormat format,
								const ServerLogConfig *config);
static void render_raw_text(Buf *buf, const Value *records);

static void
fatal(const char *fmt,...)
{
	va_list		ap;

	fprintf(stderr, "auto_explain_z_dump: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(1);
}

static void *
xmalloc(size_t size)
{
	void	   *ptr;

	if (size == 0)
		size = 1;
	ptr = malloc(size);
	if (ptr == NULL)
		fatal("out of memory");
	return ptr;
}

static void *
xrealloc(void *ptr, size_t size)
{
	void	   *out;

	if (size == 0)
		size = 1;
	out = realloc(ptr, size);
	if (out == NULL)
		fatal("out of memory");
	return out;
}

static char *
xstrdup(const char *s)
{
	if (s == NULL)
		return NULL;
	return xstrndup(s, strlen(s));
}

static char *
xstrndup(const char *s, size_t len)
{
	char	   *out = xmalloc(len + 1);

	memcpy(out, s, len);
	out[len] = '\0';
	return out;
}

static char *
xasprintf(const char *fmt,...)
{
	va_list		ap;
	va_list		ap2;
	int			n;
	char	   *out;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		fatal("formatting failed");
	out = xmalloc((size_t) n + 1);
	vsnprintf(out, (size_t) n + 1, fmt, ap2);
	va_end(ap2);
	return out;
}

static void
buf_init(Buf *buf)
{
	buf->data = NULL;
	buf->len = 0;
	buf->cap = 0;
}

static void
buf_reserve(Buf *buf, size_t needed)
{
	if (needed <= buf->cap)
		return;
	if (buf->cap == 0)
		buf->cap = 256;
	while (buf->cap < needed)
		buf->cap *= 2;
	buf->data = xrealloc(buf->data, buf->cap);
}

static void
buf_append(Buf *buf, const char *data, size_t len)
{
	buf_reserve(buf, buf->len + len + 1);
	memcpy(buf->data + buf->len, data, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
}

static void
buf_appendc(Buf *buf, char c)
{
	buf_append(buf, &c, 1);
}

static void
buf_appends(Buf *buf, const char *s)
{
	if (s)
		buf_append(buf, s, strlen(s));
}

static void
buf_appendf(Buf *buf, const char *fmt,...)
{
	va_list		ap;
	va_list		ap2;
	int			n;
	size_t		off;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		fatal("formatting failed");
	off = buf->len;
	buf_reserve(buf, buf->len + (size_t) n + 1);
	vsnprintf(buf->data + off, (size_t) n + 1, fmt, ap2);
	va_end(ap2);
	buf->len += (size_t) n;
}

static char *
buf_steal(Buf *buf)
{
	char	   *out;

	if (buf->data == NULL)
		return xstrdup("");
	out = buf->data;
	buf->data = NULL;
	buf->len = buf->cap = 0;
	return out;
}

static Value *
value_new(ValueType type)
{
	Value	   *value = xmalloc(sizeof(Value));

	memset(value, 0, sizeof(Value));
	value->type = type;
	return value;
}

static Value *
value_null(void)
{
	return value_new(V_NULL);
}

static Value *
value_bool(bool b)
{
	Value	   *value = value_new(V_BOOL);

	value->v.b = b;
	return value;
}

static Value *
value_int(int64_t i)
{
	Value	   *value = value_new(V_INT);

	value->v.i = i;
	return value;
}

static Value *
value_uint(uint64_t u)
{
	Value	   *value = value_new(V_UINT);

	value->v.u = u;
	return value;
}

static Value *
value_double(double d)
{
	Value	   *value = value_new(V_DOUBLE);

	value->v.d = d;
	return value;
}

static Value *
value_numeric(const char *s)
{
	Value	   *value = value_new(V_NUMERIC);

	value->v.s = xstrdup(s);
	return value;
}

static Value *
value_string(const char *s)
{
	Value	   *value = value_new(V_STRING);

	value->v.s = xstrdup(s ? s : "");
	return value;
}

static Value *
value_string_steal(char *s)
{
	Value	   *value;

	if (s == NULL)
		return value_null();
	value = value_new(V_STRING);
	value->v.s = s;
	return value;
}

static Value *
value_array(void)
{
	Value	   *value = value_new(V_ARRAY);

	value->v.a = xmalloc(sizeof(Array));
	memset(value->v.a, 0, sizeof(Array));
	return value;
}

static Value *
value_object(void)
{
	Value	   *value = value_new(V_OBJECT);

	value->v.o = xmalloc(sizeof(Object));
	memset(value->v.o, 0, sizeof(Object));
	return value;
}

static Value *
value_copy(const Value *value)
{
	Value	   *out;

	if (value == NULL)
		return value_null();
	out = value_new(value->type);
	switch (value->type)
	{
		case V_NULL:
			break;
		case V_BOOL:
			out->v.b = value->v.b;
			break;
		case V_INT:
			out->v.i = value->v.i;
			break;
		case V_UINT:
			out->v.u = value->v.u;
			break;
		case V_DOUBLE:
			out->v.d = value->v.d;
			break;
		case V_NUMERIC:
		case V_STRING:
			out->v.s = xstrdup(value->v.s);
			break;
		case V_ARRAY:
			out->v.a = xmalloc(sizeof(Array));
			memset(out->v.a, 0, sizeof(Array));
			for (size_t i = 0; i < value->v.a->len; i++)
				array_append(out, value_copy(value->v.a->items[i]));
			break;
		case V_OBJECT:
			out->v.o = xmalloc(sizeof(Object));
			memset(out->v.o, 0, sizeof(Object));
			for (size_t i = 0; i < value->v.o->len; i++)
				object_set(out, value->v.o->pairs[i].key,
						   value_copy(value->v.o->pairs[i].value));
			break;
	}
	return out;
}

static void
array_append(Value *array, Value *value)
{
	Array	   *a;

	if (array->type != V_ARRAY)
		fatal("internal error: array_append on non-array");
	a = array->v.a;
	if (a->len == a->cap)
	{
		a->cap = a->cap ? a->cap * 2 : 8;
		a->items = xrealloc(a->items, a->cap * sizeof(Value *));
	}
	a->items[a->len++] = value;
}

static size_t
array_len(const Value *array)
{
	return (array && array->type == V_ARRAY) ? array->v.a->len : 0;
}

static Value *
array_get(const Value *array, size_t idx)
{
	if (array == NULL || array->type != V_ARRAY || idx >= array->v.a->len)
		return NULL;
	return array->v.a->items[idx];
}

static ssize_t
object_index(const Value *object, const char *key)
{
	if (object == NULL || object->type != V_OBJECT)
		return -1;
	for (size_t i = 0; i < object->v.o->len; i++)
	{
		if (strcmp(object->v.o->pairs[i].key, key) == 0)
			return (ssize_t) i;
	}
	return -1;
}

static void
object_set(Value *object, const char *key, Value *value)
{
	Object	   *o;
	ssize_t		idx;

	if (object->type != V_OBJECT)
		fatal("internal error: object_set on non-object");
	idx = object_index(object, key);
	if (idx >= 0)
	{
		object->v.o->pairs[idx].value = value;
		return;
	}
	o = object->v.o;
	if (o->len == o->cap)
	{
		o->cap = o->cap ? o->cap * 2 : 16;
		o->pairs = xrealloc(o->pairs, o->cap * sizeof(Pair));
	}
	o->pairs[o->len].key = xstrdup(key);
	o->pairs[o->len].value = value;
	o->len++;
}

static void
object_set_append(Value *object, const char *key, Value *value)
{
	Value	   *old;

	old = object_get(object, key);
	if (old == NULL)
	{
		object_set(object, key, value);
		return;
	}
	if (old->type == V_ARRAY)
	{
		array_append(old, value);
		return;
	}
	{
		Value	   *array = value_array();

		array_append(array, old);
		array_append(array, value);
		object_set(object, key, array);
	}
}

static Value *
object_get(const Value *object, const char *key)
{
	ssize_t		idx = object_index(object, key);

	return idx >= 0 ? object->v.o->pairs[idx].value : NULL;
}

static bool
object_has(const Value *object, const char *key)
{
	return object_get(object, key) != NULL;
}

static Value *
object_remove(Value *object, const char *key)
{
	ssize_t		idx;
	Value	   *value;
	Object	   *o;

	if (object == NULL || object->type != V_OBJECT)
		return NULL;
	idx = object_index(object, key);
	if (idx < 0)
		return NULL;
	o = object->v.o;
	value = o->pairs[idx].value;
	for (size_t i = (size_t) idx + 1; i < o->len; i++)
		o->pairs[i - 1] = o->pairs[i];
	o->len--;
	return value;
}

static int64_t
value_i64(const Value *value, int64_t def)
{
	if (value == NULL)
		return def;
	switch (value->type)
	{
		case V_INT:
			return value->v.i;
		case V_UINT:
			return (int64_t) value->v.u;
		case V_DOUBLE:
			return (int64_t) value->v.d;
		case V_NUMERIC:
		case V_STRING:
			return strtoll(value->v.s, NULL, 10);
		case V_BOOL:
			return value->v.b ? 1 : 0;
		default:
			return def;
	}
}

static uint64_t
value_u64(const Value *value, uint64_t def)
{
	if (value == NULL)
		return def;
	switch (value->type)
	{
		case V_INT:
			return (uint64_t) value->v.i;
		case V_UINT:
			return value->v.u;
		case V_DOUBLE:
			return (uint64_t) value->v.d;
		case V_NUMERIC:
		case V_STRING:
			return strtoull(value->v.s, NULL, 10);
		case V_BOOL:
			return value->v.b ? 1 : 0;
		default:
			return def;
	}
}

static double
value_double_as(const Value *value, double def)
{
	if (value == NULL)
		return def;
	switch (value->type)
	{
		case V_INT:
			return (double) value->v.i;
		case V_UINT:
			return (double) value->v.u;
		case V_DOUBLE:
			return value->v.d;
		case V_NUMERIC:
		case V_STRING:
			return strtod(value->v.s, NULL);
		case V_BOOL:
			return value->v.b ? 1.0 : 0.0;
		default:
			return def;
	}
}

static const char *
value_cstr(const Value *value)
{
	if (value == NULL || value->type == V_NULL)
		return NULL;
	if (value->type == V_STRING || value->type == V_NUMERIC)
		return value->v.s;
	return NULL;
}

static bool
value_truthy(const Value *value)
{
	if (value == NULL || value->type == V_NULL)
		return false;
	if (value->type == V_BOOL)
		return value->v.b;
	if (value->type == V_STRING || value->type == V_NUMERIC)
		return value->v.s && value->v.s[0] != '\0';
	if (value->type == V_ARRAY)
		return value->v.a->len > 0;
	if (value->type == V_OBJECT)
		return value->v.o->len > 0;
	if (value->type == V_DOUBLE)
		return value->v.d != 0.0;
	return value_i64(value, 0) != 0;
}

static Binary
reader_take(Reader *r, size_t len)
{
	Binary		out;

	if (len > r->len - r->pos)
		fatal("unexpected end of data");
	out.data = (uint8_t *) (r->data + r->pos);
	out.len = len;
	r->pos += len;
	return out;
}

static uint8_t
reader_u8(Reader *r)
{
	return reader_take(r, 1).data[0];
}

static uint16_t
reader_u16(Reader *r)
{
	Binary		b = reader_take(r, 2);

	return (uint16_t) b.data[0] | ((uint16_t) b.data[1] << 8);
}

static uint32_t
reader_u32(Reader *r)
{
	Binary		b = reader_take(r, 4);

	return ((uint32_t) b.data[0]) |
		((uint32_t) b.data[1] << 8) |
		((uint32_t) b.data[2] << 16) |
		((uint32_t) b.data[3] << 24);
}

static int32_t
reader_i32(Reader *r)
{
	return (int32_t) reader_u32(r);
}

static uint64_t
reader_u64(Reader *r)
{
	uint64_t	lo = reader_u32(r);
	uint64_t	hi = reader_u32(r);

	return lo | (hi << 32);
}

static int64_t
reader_i64(Reader *r)
{
	return (int64_t) reader_u64(r);
}

static double
reader_double(Reader *r)
{
	uint64_t	u = reader_u64(r);
	double		d;

	memcpy(&d, &u, sizeof(d));
	return d;
}

static uint64_t
reader_uint_sized(Reader *r, unsigned code)
{
	switch (code)
	{
		case 0:
			return reader_u8(r);
		case 1:
			return reader_u16(r);
		case 2:
			return reader_u32(r);
		case 3:
			return reader_u64(r);
		default:
			fatal("bad integer size code %u", code);
	}
}

static int64_t
reader_int_sized(Reader *r, unsigned code)
{
	uint64_t	n = reader_uint_sized(r, code);
	unsigned	bits = (1U << code) * 8U;
	uint64_t	sign = UINT64_C(1) << (bits - 1);

	if (bits < 64 && (n & sign))
		n -= UINT64_C(1) << bits;
	return (int64_t) n;
}

static char *
reader_string(Reader *r)
{
	uint32_t	len;
	Binary		b;

	if (r->version >= 4)
	{
		uint8_t		ctrl = reader_u8(r);

		if (ctrl == STRING_NULL_CTRL)
			return NULL;
		len = (uint32_t) reader_uint_sized(r, ctrl & 0x03);
	}
	else
	{
		len = reader_u32(r);
		if (len == STRING_NULL)
			return NULL;
	}
	b = reader_take(r, len);
	return xstrndup((const char *) b.data, b.len);
}

static Binary
read_file(const char *path)
{
	FILE	   *f;
	struct stat st;
	Binary		out;
	size_t		nread;

	if (stat(path, &st) != 0)
		fatal("%s: %s", path, strerror(errno));
	if (st.st_size < 0)
		fatal("%s: bad file size", path);
	f = fopen(path, "rb");
	if (f == NULL)
		fatal("%s: %s", path, strerror(errno));
	out.len = (size_t) st.st_size;
	out.data = xmalloc(out.len ? out.len : 1);
	nread = fread(out.data, 1, out.len, f);
	if (nread != out.len)
		fatal("%s: read failed", path);
	fclose(f);
	return out;
}

static uint32_t
crc32c_sw(const uint8_t *data, size_t len)
{
	uint32_t	crc = UINT32_C(0xffffffff);

	for (size_t i = 0; i < len; i++)
	{
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (UINT32_C(0x82f63b78) & (uint32_t) -(int32_t) (crc & 1));
	}
	return crc ^ UINT32_C(0xffffffff);
}

static Binary
decompress_payload(uint16_t method, const uint8_t *data, size_t len,
				   size_t expected_len)
{
	Binary		out;

	if (method == COMPRESSION_NONE)
	{
		if (expected_len != len)
			fatal("uncompressed payload length mismatch");
		out.data = xmalloc(len ? len : 1);
		memcpy(out.data, data, len);
		out.len = len;
		return out;
	}
	if (method == COMPRESSION_LZ4)
	{
#ifdef USE_LZ4
		int			rc;

		out.data = xmalloc(expected_len ? expected_len : 1);
		rc = LZ4_decompress_safe((const char *) data, (char *) out.data,
								 (int) len, (int) expected_len);
		if (rc < 0 || (size_t) rc != expected_len)
			fatal("lz4 decompression failed");
		out.len = (size_t) rc;
		return out;
#else
		fatal("lz4 compressed record requires PostgreSQL built with lz4");
#endif
	}
	if (method == COMPRESSION_ZSTD)
	{
#ifdef USE_ZSTD
		size_t		rc;

		out.data = xmalloc(expected_len ? expected_len : 1);
		rc = ZSTD_decompress(out.data, expected_len, data, len);
		if (ZSTD_isError(rc) || rc != expected_len)
			fatal("zstd decompression failed");
		out.len = rc;
		return out;
#else
		fatal("zstd compressed record requires PostgreSQL built with zstd");
#endif
	}
	fatal("unknown compression method %u", method);
}

static Binary
decompress_file_body(uint16_t method, const uint8_t *data, size_t len)
{
	Binary		out;

	if (method == COMPRESSION_NONE)
	{
		out.data = xmalloc(len ? len : 1);
		memcpy(out.data, data, len);
		out.len = len;
		return out;
	}
	if (method == COMPRESSION_LZ4)
	{
#ifdef USE_LZ4
		LZ4F_decompressionContext_t ctx;
		size_t		rc;
		size_t		src_pos = 0;
		Buf			buf;
		char		tmp[65536];

		rc = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
		if (LZ4F_isError(rc))
			fatal("lz4 frame setup failed");
		buf_init(&buf);
		while (src_pos < len)
		{
			size_t		src_size = len - src_pos;
			size_t		dst_size = sizeof(tmp);

			rc = LZ4F_decompress(ctx, tmp, &dst_size, data + src_pos,
								  &src_size, NULL);
			if (LZ4F_isError(rc))
				fatal("lz4 frame decompression failed");
			src_pos += src_size;
			if (dst_size > 0)
				buf_append(&buf, tmp, dst_size);
			if (rc == 0 && src_size == 0)
				break;
		}
		LZ4F_freeDecompressionContext(ctx);
		out.len = buf.len;
		out.data = (uint8_t *) buf_steal(&buf);
		return out;
#else
		fatal("lz4 file stream requires PostgreSQL built with lz4");
#endif
	}
	if (method == COMPRESSION_ZSTD)
	{
#ifdef USE_ZSTD
		ZSTD_DStream *stream;
		ZSTD_inBuffer input;
		Buf			buf;
		char		tmp[65536];

		stream = ZSTD_createDStream();
		if (stream == NULL)
			fatal("zstd stream setup failed");
		input.src = data;
		input.size = len;
		input.pos = 0;
		buf_init(&buf);
		while (input.pos < input.size)
		{
			ZSTD_outBuffer output;
			size_t		rc;

			output.dst = tmp;
			output.size = sizeof(tmp);
			output.pos = 0;
			rc = ZSTD_decompressStream(stream, &output, &input);
			if (ZSTD_isError(rc))
				fatal("zstd stream decompression failed");
			if (output.pos > 0)
				buf_append(&buf, tmp, output.pos);
		}
		ZSTD_freeDStream(stream);
		out.len = buf.len;
		out.data = (uint8_t *) buf_steal(&buf);
		return out;
#else
		fatal("zstd file stream requires PostgreSQL built with zstd");
#endif
	}
	fatal("unknown compression method %u", method);
}

static const char *
compression_name(uint16_t method)
{
	switch (method)
	{
		case COMPRESSION_NONE:
			return "none";
		case COMPRESSION_LZ4:
			return "lz4";
		case COMPRESSION_ZSTD:
			return "zstd";
		default:
			return NULL;
	}
}

static const char *
profile_name(unsigned profile)
{
	switch (profile)
	{
		case PROFILE_SIMPLE:
			return "simple";
		case PROFILE_FULL:
			return "full";
		default:
			return NULL;
	}
}

static const char *
template_mode_name(unsigned mode)
{
	switch (mode)
	{
		case TEMPLATE_NONE:
			return "none";
		case TEMPLATE_DEFINE:
			return "define";
		case TEMPLATE_REF:
			return "ref";
		default:
			return NULL;
	}
}

static const char *
relationship_name(unsigned rel)
{
	switch (rel)
	{
		case 0:
			return NULL;
		case 1:
			return "Outer";
		case 2:
			return "Inner";
		case 3:
			return "Member";
		case 4:
			return "Subquery";
		case 5:
			return "InitPlan";
		case 6:
			return "SubPlan";
		case 7:
			return "Custom";
		default:
			return NULL;
	}
}

static const char *
detail_label(unsigned code)
{
	static const char *labels[] = {
		NULL,
		"Output", "Filter", "Index Cond", "Order By", "Recheck Cond",
		"TID Cond", "Join Filter", "Hash Cond", "Merge Cond", "Sort Key",
		"Presorted Key", "Group Key", "Hash Key", "Function Call",
		"Table Function Call", "One-Time Filter", "Run Condition",
		"Heap Fetches", "Workers Planned", "Workers Launched",
		"Rows Removed by Filter", "Rows Removed by Join Filter",
		"Rows Removed by Index Recheck", "Inner Unique", "Sort Info",
		"Incremental Sort Groups", "Hash Info", "Storage Info",
		"Cache Key", "Cache Mode", "Memoize Stats", "Planned Partitions",
		"HashAgg Stats", "Index Searches", "Heap Blocks",
		"Subplans Removed", "Conflict Resolution", "Conflict Arbiter Index",
		"Conflict Filter", "Rows Removed by Conflict Filter",
		"Conflict Tuples", "Merge Tuples", "Trigger", "JIT",
		"Extension Explain", "Merge Actions", "Planning",
		"Custom Plan Provider", "Single Copy", "Alias", "Function Name",
		"Table Function Name", "CTE Name", "Tuplestore Name",
		"Sampling Method", "Sampling Parameters", "Repeatable Seed",
		"Schema", "Window", "Subplan Name",
	};

	if (code > 0 && code < sizeof(labels) / sizeof(labels[0]))
		return labels[code];
	return NULL;
}

static const char *
node_name(unsigned tag)
{
	switch (tag)
	{
		case 331: return "Result";
		case 332: return "ProjectSet";
		case 333: return "ModifyTable";
		case 334: return "Append";
		case 335: return "Merge Append";
		case 336: return "Recursive Union";
		case 337: return "BitmapAnd";
		case 338: return "BitmapOr";
		case 339: return "Seq Scan";
		case 340: return "Sample Scan";
		case 341: return "Index Scan";
		case 342: return "Index Only Scan";
		case 343: return "Bitmap Index Scan";
		case 344: return "Bitmap Heap Scan";
		case 345: return "Tid Scan";
		case 346: return "Tid Range Scan";
		case 347: return "Subquery Scan";
		case 348: return "Function Scan";
		case 349: return "Values Scan";
		case 350: return "Table Function Scan";
		case 351: return "CTE Scan";
		case 352: return "Named Tuplestore Scan";
		case 353: return "WorkTable Scan";
		case 354: return "Foreign Scan";
		case 355: return "Custom Scan";
		case 356: return "Nested Loop";
		case 358: return "Merge Join";
		case 359: return "Hash Join";
		case 360: return "Materialize";
		case 361: return "Memoize";
		case 362: return "Sort";
		case 363: return "Incremental Sort";
		case 364: return "Group";
		case 365: return "Aggregate";
		case 366: return "WindowAgg";
		case 367: return "Unique";
		case 368: return "Gather";
		case 369: return "Gather Merge";
		case 370: return "Hash";
		case 371: return "SetOp";
		case 372: return "LockRows";
		case 373: return "Limit";
		default: return NULL;
	}
}

static bool
detail_is_dynamic(unsigned code)
{
	switch (code)
	{
		case 18:
		case 20:
		case 21:
		case 22:
		case 23:
		case 25:
		case 26:
		case 27:
		case 28:
		case 31:
		case 33:
		case 34:
		case 35:
		case 40:
		case 41:
		case 42:
		case 45:
			return true;
		default:
			return false;
	}
}

static char *
pg_timestamp_to_iso(int64_t ts)
{
	int64_t		unix_us = ts + INT64_C(946684800000000);
	time_t		sec = (time_t) (unix_us / 1000000);
	int64_t		usec = unix_us % 1000000;
	struct tm	tm;
	char		base[64];

	if (usec < 0)
	{
		usec += 1000000;
		sec--;
	}
	gmtime_r(&sec, &tm);
	strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
	if (usec == 0)
		return xasprintf("%s+00:00", base);
	return xasprintf("%s.%06" PRId64 "+00:00", base, usec);
}

static int64_t
days_from_civil(int year, unsigned month, unsigned day)
{
	int			era;
	unsigned	yoe;
	unsigned	doy;
	unsigned	doe;

	year -= month <= 2;
	era = (year >= 0 ? year : year - 399) / 400;
	yoe = (unsigned) (year - era * 400);
	doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (int64_t) era * 146097 + (int64_t) doe - 719468;
}

static bool
parse_iso_timestamp(const char *s, ParsedTimestamp *out)
{
	int			year;
	int			month;
	int			day;
	int			hour;
	int			minute;
	int			second;
	int			tz_hour = 0;
	int			tz_minute = 0;
	int			tz_sign = 1;
	int			n = 0;
	int			usec = 0;
	const char *p;
	int64_t		epoch;
	int			offset;

	memset(out, 0, sizeof(*out));
	if (s == NULL)
		return false;
	if (sscanf(s, "%d-%d-%dT%d:%d:%d%n",
			   &year, &month, &day, &hour, &minute, &second, &n) != 6)
		return false;
	p = s + n;
	if (*p == '.')
	{
		int			digits = 0;

		p++;
		while (isdigit((unsigned char) *p))
		{
			if (digits < 6)
				usec = usec * 10 + (*p - '0');
			digits++;
			p++;
		}
		while (digits > 0 && digits < 6)
		{
			usec *= 10;
			digits++;
		}
	}
	if (*p == 'Z')
		p++;
	else if (*p == '+' || *p == '-')
	{
		tz_sign = (*p == '-') ? -1 : 1;
		p++;
		if (sscanf(p, "%d:%d", &tz_hour, &tz_minute) != 2)
			return false;
	}
	else
		return false;

	if (month < 1 || month > 12 || day < 1 || day > 31 ||
		hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
		second < 0 || second > 60)
		return false;

	epoch = days_from_civil(year, (unsigned) month, (unsigned) day) * 86400 +
		hour * 3600 + minute * 60 + second;
	offset = tz_sign * (tz_hour * 3600 + tz_minute * 60);
	out->sec = (time_t) (epoch - offset);
	out->usec = usec;
	out->valid = true;
	return true;
}

static char *
format_log_timestamp_ms(const char *s)
{
	ParsedTimestamp ts;
	struct tm	tm;
	char		base[64];
	char		zone[32];

	if (!parse_iso_timestamp(s, &ts))
		return xstrdup("");
	localtime_r(&ts.sec, &tm);
	strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tm);
	if (strftime(zone, sizeof(zone), "%Z", &tm) == 0)
		zone[0] = '\0';
	return xasprintf("%s.%03d %s", base, ts.usec / 1000, zone);
}

static char *
format_log_timestamp(const char *s)
{
	ParsedTimestamp ts;
	struct tm	tm;
	char		base[64];

	if (!parse_iso_timestamp(s, &ts))
		return xstrdup("");
	localtime_r(&ts.sec, &tm);
	strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S %Z", &tm);
	return xstrdup(base);
}

static char *
format_log_epoch_ms(const char *s)
{
	ParsedTimestamp ts;

	if (!parse_iso_timestamp(s, &ts))
		return xstrdup("");
	return xasprintf("%" PRId64 ".%03d", (int64_t) ts.sec, ts.usec / 1000);
}

static char *
format_session_id(const Value *ctx)
{
	uint64_t	start_epoch = value_u64(object_get(ctx, "Backend Start Epoch"), 0);
	uint64_t	pid = value_u64(object_get(ctx, "PID"), 0);

	return xasprintf("%" PRIx64 ".%x", start_epoch, (unsigned int) pid);
}

static ServerLogConfig
log_config_from_options(const Options *opts)
{
	ServerLogConfig config;

	memset(&config, 0, sizeof(config));
	config.log_line_prefix = opts->log_line_prefix;
	config.log_timezone = opts->log_timezone;
	if (strcmp(opts->log_error_verbosity, "verbose") == 0)
		config.verbose = true;
	else if (strcmp(opts->log_error_verbosity, "default") == 0 ||
			 strcmp(opts->log_error_verbosity, "terse") == 0)
		config.verbose = false;
	else
		fatal("unknown log_error_verbosity: %s", opts->log_error_verbosity);

	if (config.log_timezone != NULL && config.log_timezone[0] != '\0')
	{
		setenv("TZ", config.log_timezone, 1);
		tzset();
	}
	return config;
}

static char *
numeric_fixed(double value, int digits)
{
	return xasprintf("%.*f", digits, value);
}

static int64_t
pg_signed_i64(uint64_t value)
{
	return (int64_t) value;
}

static Value *
parse_details(Reader *r)
{
	uint16_t	count = reader_u16(r);
	Value	   *details = value_array();

	for (uint16_t i = 0; i < count; i++)
	{
		uint16_t	code = reader_u16(r);
		uint8_t		typ = reader_u8(r);
		Value	   *detail = value_object();
		Value	   *value = NULL;
		const char *label = detail_label(code);

		switch (typ)
		{
			case DETAIL_STRING:
				value = value_string_steal(reader_string(r));
				break;
			case DETAIL_STRING_LIST:
				{
					uint32_t	n = reader_u32(r);

					value = value_array();
					for (uint32_t j = 0; j < n; j++)
						array_append(value, value_string_steal(reader_string(r)));
				}
				break;
			case DETAIL_DOUBLE:
				value = value_double(reader_double(r));
				break;
			case DETAIL_INT64:
				value = value_int(reader_i64(r));
				break;
			case DETAIL_UINT64:
				value = value_uint(reader_u64(r));
				break;
			case DETAIL_BOOL:
				value = value_bool(reader_u8(r) != 0);
				break;
			default:
				fatal("unknown detail type %u", typ);
		}

		object_set(detail, "code", value_int(code));
		object_set(detail, "label",
				   value_string(label ? label : xasprintf("Detail %u", code)));
		object_set(detail, "value", value);
		array_append(details, detail);
	}
	return details;
}

static Value *
parse_actual(Reader *r, uint16_t flags)
{
	Value	   *actual;

	if (r->version >= 4 && !(flags & NODE_HAS_ACTUAL))
		return NULL;
	actual = value_object();
	object_set(actual, "Startup Time", value_double(reader_i64(r) / 1000.0));
	object_set(actual, "Total Time", value_double(reader_i64(r) / 1000.0));
	object_set(actual, "Actual Rows", value_double(reader_double(r)));
	object_set(actual, "Actual Loops", value_double(reader_double(r)));
	if (!(flags & NODE_HAS_ACTUAL))
		return NULL;
	return actual;
}

static Value *
parse_buffers(Reader *r)
{
	static const char *names[] = {
		"Shared Hit Blocks", "Shared Read Blocks", "Shared Dirtied Blocks",
		"Shared Written Blocks", "Local Hit Blocks", "Local Read Blocks",
		"Local Dirtied Blocks", "Local Written Blocks", "Temp Read Blocks",
		"Temp Written Blocks", "Shared Read Time", "Shared Write Time",
		"Local Read Time", "Local Write Time", "Temp Read Time",
		"Temp Write Time",
	};
	Value	   *out = value_object();

	for (int i = 0; i < 10; i++)
		object_set(out, names[i], value_int(reader_i64(r)));
	for (int i = 10; i < 16; i++)
		object_set(out, names[i], value_double(reader_i64(r) / 1000.0));
	return out;
}

static Value *
parse_wal(Reader *r)
{
	Value	   *out = value_object();

	object_set(out, "WAL Records", value_int(reader_i64(r)));
	object_set(out, "WAL FPI", value_int(reader_i64(r)));
	object_set(out, "WAL Bytes", value_uint(reader_u64(r)));
	object_set(out, "WAL Buffers Full", value_int(reader_i64(r)));
	return out;
}

static void
parse_identity(Reader *r, Value *node)
{
	uint8_t		ctrl = reader_u8(r);
	uint8_t		flags = r->version >= 4 ? (ctrl & 0x03) : ctrl;

	if (flags & OBJ_RELATION)
	{
		if (r->version >= 4)
			object_set(node, "Relation OID",
					   value_uint(reader_uint_sized(r, (ctrl >> 2) & 0x03)));
		else
			object_set(node, "Relation OID", value_uint(reader_u32(r)));
		object_set(node, "Relation Name", value_string_steal(reader_string(r)));
		object_set(node, "Alias", value_string_steal(reader_string(r)));
		if (r->version >= 9)
		{
			char	   *schema = reader_string(r);

			if (schema != NULL)
				object_set(node, "Schema", value_string_steal(schema));
		}
	}
	if (flags & OBJ_INDEX)
	{
		if (r->version >= 4)
			object_set(node, "Index OID",
					   value_uint(reader_uint_sized(r, (ctrl >> 4) & 0x03)));
		else
			object_set(node, "Index OID", value_uint(reader_u32(r)));
		object_set(node, "Index Name", value_string_steal(reader_string(r)));
	}
}

static void
object_update(Value *dst, Value *src)
{
	if (src == NULL || src->type != V_OBJECT)
		return;
	for (size_t i = 0; i < src->v.o->len; i++)
		object_set(dst, src->v.o->pairs[i].key, src->v.o->pairs[i].value);
}

static Value *
parse_full_plan_node(Reader *r)
{
	uint8_t		relationship = reader_u8(r);
	uint16_t	node_tag = reader_u16(r);
	uint16_t	flags = reader_u16(r);
	uint8_t		extra_kind = reader_u8(r);
	uint64_t	extra1;
	uint64_t	extra2;
	unsigned	width_code = 0;
	unsigned	child_count_code = 0;
	Value	   *node = value_object();
	Value	   *plans = value_array();
	const char *relname = relationship_name(relationship);
	const char *ntype = node_name(node_tag);
	Value	   *actual;

	if (r->version >= 4)
	{
		uint8_t		size_flags = reader_u8(r);

		extra1 = reader_uint_sized(r, size_flags & 0x03);
		extra2 = reader_uint_sized(r, (size_flags >> 2) & 0x03);
		width_code = (size_flags >> 4) & 0x03;
		child_count_code = (size_flags >> 6) & 0x03;
	}
	else
	{
		extra1 = reader_u32(r);
		extra2 = reader_u32(r);
	}

	object_set(node, "Relationship", relname ? value_string(relname) : value_null());
	object_set(node, "Node Tag", value_int(node_tag));
	object_set(node, "Node Type", ntype ? value_string(ntype) :
			   value_string_steal(xasprintf("T_%u", node_tag)));
	object_set(node, "Flags", value_int(flags));
	object_set(node, "Startup Cost", value_double(reader_double(r)));
	object_set(node, "Total Cost", value_double(reader_double(r)));
	object_set(node, "Plan Rows", value_double(reader_double(r)));
	object_set(node, "Plan Width",
			   r->version >= 4 ? value_int(reader_int_sized(r, width_code)) :
			   value_int(reader_i32(r)));
	object_set(node, "Plans", plans);
	if (extra_kind)
	{
		object_set(node, "Extra Kind", value_int(extra_kind));
		object_set(node, "Extra1", value_uint(extra1));
		object_set(node, "Extra2", value_uint(extra2));
	}
	actual = parse_actual(r, flags);
	object_update(node, actual);
	if (flags & NODE_NEVER_EXECUTED)
		object_set(node, "Never Executed", value_bool(true));
	if (flags & NODE_HAS_BUFFERS)
		object_set(node, "Buffers", parse_buffers(r));
	if (flags & NODE_HAS_WAL)
		object_set(node, "WAL", parse_wal(r));
	parse_identity(r, node);
	object_set(node, "Details", parse_details(r));
	{
		uint64_t	child_count = r->version >= 4 ?
			reader_uint_sized(r, child_count_code) : reader_u32(r);

		for (uint64_t i = 0; i < child_count; i++)
			array_append(plans, parse_full_plan_node(r));
	}
	return node;
}

static Value *
parse_metric_node(Reader *r)
{
	uint16_t	node_tag = reader_u16(r);
	uint16_t	flags = reader_u16(r);
	unsigned	width_code = 0;
	unsigned	child_count_code = 0;
	Value	   *node = value_object();
	Value	   *plans = value_array();
	const char *ntype = node_name(node_tag);
	Value	   *actual;

	if (r->version >= 4)
	{
		uint8_t		size_flags = reader_u8(r);

		width_code = size_flags & 0x03;
		child_count_code = (size_flags >> 2) & 0x03;
	}
	object_set(node, "Node Tag", value_int(node_tag));
	object_set(node, "Node Type", ntype ? value_string(ntype) :
			   value_string_steal(xasprintf("T_%u", node_tag)));
	object_set(node, "Flags", value_int(flags));
	object_set(node, "Startup Cost", value_double(reader_double(r)));
	object_set(node, "Total Cost", value_double(reader_double(r)));
	object_set(node, "Plan Rows", value_double(reader_double(r)));
	object_set(node, "Plan Width",
			   r->version >= 4 ? value_int(reader_int_sized(r, width_code)) :
			   value_int(reader_i32(r)));
	object_set(node, "Plans", plans);
	actual = parse_actual(r, flags);
	object_update(node, actual);
	if (flags & NODE_NEVER_EXECUTED)
		object_set(node, "Never Executed", value_bool(true));
	if (flags & NODE_HAS_BUFFERS)
		object_set(node, "Buffers", parse_buffers(r));
	if (flags & NODE_HAS_WAL)
		object_set(node, "WAL", parse_wal(r));
	object_set(node, "Details", parse_details(r));
	{
		uint64_t	child_count = r->version >= 4 ?
			reader_uint_sized(r, child_count_code) : reader_u32(r);

		for (uint64_t i = 0; i < child_count; i++)
			array_append(plans, parse_metric_node(r));
	}
	return node;
}

static Value *
strip_dynamic_details(const Value *node)
{
	Value	   *out = value_copy(node);
	Value	   *details;
	Value	   *newdetails = value_array();
	static const char *dynamic_keys[] = {
		"Startup Time", "Total Time", "Actual Rows", "Actual Loops",
		"Never Executed", "Buffers", "WAL",
	};

	for (size_t i = 0; i < sizeof(dynamic_keys) / sizeof(dynamic_keys[0]); i++)
		object_remove(out, dynamic_keys[i]);

	details = object_get(out, "Details");
	if (details && details->type == V_ARRAY)
	{
		for (size_t i = 0; i < details->v.a->len; i++)
		{
			Value	   *detail = details->v.a->items[i];
			unsigned	code = (unsigned) value_i64(object_get(detail, "code"), 0);

			if (!detail_is_dynamic(code))
				array_append(newdetails, value_copy(detail));
		}
		object_set(out, "Details", newdetails);
	}
	{
		Value	   *plans = object_get(out, "Plans");
		Value	   *newplans = value_array();

		if (plans && plans->type == V_ARRAY)
		{
			for (size_t i = 0; i < plans->v.a->len; i++)
				array_append(newplans, strip_dynamic_details(plans->v.a->items[i]));
			object_set(out, "Plans", newplans);
		}
	}
	return out;
}

static Value *
merge_template_metrics(const Value *template_plan, const Value *metrics)
{
	Value	   *merged = value_copy(template_plan);
	static const char *replace_keys[] = {
		"Flags", "Startup Cost", "Total Cost", "Plan Rows", "Plan Width",
		"Startup Time", "Total Time", "Actual Rows", "Actual Loops",
		"Never Executed", "Buffers", "WAL",
	};
	Value	   *details;
	Value	   *metric_details;
	Value	   *newdetails = value_array();
	Value	   *merged_plans;
	Value	   *metric_plans;
	Value	   *newplans = value_array();

	for (size_t i = 0; i < sizeof(replace_keys) / sizeof(replace_keys[0]); i++)
	{
		Value	   *v = object_get(metrics, replace_keys[i]);

		if (v)
			object_set(merged, replace_keys[i], value_copy(v));
		else if (strcmp(replace_keys[i], "Startup Time") == 0 ||
				 strcmp(replace_keys[i], "Total Time") == 0 ||
				 strcmp(replace_keys[i], "Actual Rows") == 0 ||
				 strcmp(replace_keys[i], "Actual Loops") == 0 ||
				 strcmp(replace_keys[i], "Never Executed") == 0 ||
				 strcmp(replace_keys[i], "Buffers") == 0 ||
				 strcmp(replace_keys[i], "WAL") == 0)
			object_remove(merged, replace_keys[i]);
	}

	details = object_get(merged, "Details");
	if (details && details->type == V_ARRAY)
	{
		for (size_t i = 0; i < details->v.a->len; i++)
		{
			Value	   *detail = details->v.a->items[i];
			unsigned	code = (unsigned) value_i64(object_get(detail, "code"), 0);

			if (!detail_is_dynamic(code))
				array_append(newdetails, value_copy(detail));
		}
	}
	metric_details = object_get(metrics, "Details");
	if (metric_details && metric_details->type == V_ARRAY)
	{
		for (size_t i = 0; i < metric_details->v.a->len; i++)
			array_append(newdetails, value_copy(metric_details->v.a->items[i]));
	}
	object_set(merged, "Details", newdetails);

	merged_plans = object_get(merged, "Plans");
	metric_plans = object_get(metrics, "Plans");
	if (merged_plans && metric_plans &&
		merged_plans->type == V_ARRAY && metric_plans->type == V_ARRAY)
	{
		size_t		n = merged_plans->v.a->len < metric_plans->v.a->len ?
			merged_plans->v.a->len : metric_plans->v.a->len;

		for (size_t i = 0; i < n; i++)
			array_append(newplans,
						 merge_template_metrics(merged_plans->v.a->items[i],
												metric_plans->v.a->items[i]));
		object_set(merged, "Plans", newplans);
	}
	return merged;
}

static const char *
list_str(const Value *array, size_t idx)
{
	Value	   *v = array_get(array, idx);

	return value_cstr(v) ? value_cstr(v) : "";
}

static int64_t
list_i64(const Value *array, size_t idx)
{
	return strtoll(list_str(array, idx), NULL, 10);
}

static double
list_double(const Value *array, size_t idx)
{
	return strtod(list_str(array, idx), NULL);
}

static bool
list_bool(const Value *array, size_t idx)
{
	return strcmp(list_str(array, idx), "true") == 0;
}

static Value *
detail_to_property(const Value *detail)
{
	unsigned	code = (unsigned) value_i64(object_get(detail, "code"), 0);
	Value	   *value = object_get(detail, "value");

	if (value && value->type == V_ARRAY && code == 25 && array_len(value) == 4)
	{
		Value	   *out = value_object();

		object_set(out, "Worker", value_int(list_i64(value, 0)));
		object_set(out, "Sort Method", value_string(list_str(value, 1)));
		object_set(out, "Sort Space Type", value_string(list_str(value, 2)));
		object_set(out, "Sort Space Used", value_int(list_i64(value, 3)));
		return out;
	}
	if (value && value->type == V_ARRAY && code == 26 && array_len(value) == 9)
	{
		Value	   *out = value_object();
		Value	   *methods = value_array();
		char	   *tmp = xstrdup(list_str(value, 3));
		char	   *tok;
		char	   *saveptr = NULL;

		for (tok = strtok_r(tmp, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr))
			if (tok[0] != '\0')
				array_append(methods, value_string(tok));
		object_set(out, "Worker", value_int(list_i64(value, 0)));
		object_set(out, "Group Type", value_string(list_str(value, 1)));
		object_set(out, "Group Count", value_int(list_i64(value, 2)));
		object_set(out, "Sort Methods Used", methods);
		object_set(out, "Memory Space Type", value_string(list_str(value, 4)));
		object_set(out, "Average Memory", value_int(list_i64(value, 5)));
		object_set(out, "Peak Memory", value_int(list_i64(value, 6)));
		object_set(out, "Average Disk", value_int(list_i64(value, 7)));
		object_set(out, "Peak Disk", value_int(list_i64(value, 8)));
		return out;
	}
	if (value && value->type == V_ARRAY && code == 27 && array_len(value) == 5)
	{
		Value	   *out = value_object();

		object_set(out, "Hash Buckets", value_int(list_i64(value, 0)));
		object_set(out, "Original Hash Buckets", value_int(list_i64(value, 1)));
		object_set(out, "Hash Batches", value_int(list_i64(value, 2)));
		object_set(out, "Original Hash Batches", value_int(list_i64(value, 3)));
		object_set(out, "Peak Memory Usage", value_int(list_i64(value, 4)));
		return out;
	}
	if (value && value->type == V_ARRAY && code == 28 && array_len(value) == 2)
	{
		Value	   *out = value_object();

		object_set(out, "Storage", value_string(list_str(value, 0)));
		object_set(out, "Maximum Storage", value_int(list_i64(value, 1)));
		return out;
	}
	if (value && value->type == V_ARRAY && code == 31 && array_len(value) == 6)
	{
		Value	   *out = value_object();

		object_set(out, "Worker", value_int(list_i64(value, 0)));
		object_set(out, "Cache Hits", value_int(list_i64(value, 1)));
		object_set(out, "Cache Misses", value_int(list_i64(value, 2)));
		object_set(out, "Cache Evictions", value_int(list_i64(value, 3)));
		object_set(out, "Cache Overflows", value_int(list_i64(value, 4)));
		object_set(out, "Peak Memory Usage", value_int(list_i64(value, 5)));
		return out;
	}
	if (value && value->type == V_ARRAY && code == 33 && array_len(value) == 4)
	{
		Value	   *out = value_object();

		object_set(out, "Worker", value_int(list_i64(value, 0)));
		object_set(out, "HashAgg Batches", value_int(list_i64(value, 1)));
		object_set(out, "Peak Memory Usage", value_int(list_i64(value, 2)));
		object_set(out, "Disk Usage", value_int(list_i64(value, 3)));
		return out;
	}
	if (value && value->type == V_ARRAY && code == 35 && array_len(value) == 3)
	{
		Value	   *out = value_object();

		object_set(out, "Worker", value_int(list_i64(value, 0)));
		object_set(out, "Exact Heap Blocks", value_int(list_i64(value, 1)));
		object_set(out, "Lossy Heap Blocks", value_int(list_i64(value, 2)));
		return out;
	}
	if (value && value->type == V_ARRAY && code == 41 && array_len(value) == 2)
	{
		Value	   *out = value_object();

		object_set(out, "Tuples Inserted", value_double(list_double(value, 0)));
		object_set(out, "Conflicting Tuples", value_double(list_double(value, 1)));
		return out;
	}
	if (value && value->type == V_ARRAY && code == 42 && array_len(value) == 4)
	{
		Value	   *out = value_object();

		object_set(out, "Tuples Inserted", value_double(list_double(value, 0)));
		object_set(out, "Tuples Updated", value_double(list_double(value, 1)));
		object_set(out, "Tuples Deleted", value_double(list_double(value, 2)));
		object_set(out, "Tuples Skipped", value_double(list_double(value, 3)));
		return out;
	}
	if (value && value->type == V_ARRAY && code == 43 && array_len(value) == 5)
	{
		Value	   *out = value_object();

		object_set(out, "Trigger Name", value_string(list_str(value, 0)));
		object_set(out, "Constraint Name", value_string(list_str(value, 1)));
		object_set(out, "Relation", value_string(list_str(value, 2)));
		object_set(out, "Time", value_double(list_i64(value, 3) / 1000.0));
		object_set(out, "Calls", value_double(list_double(value, 4)));
		return out;
	}
	if (value && value->type == V_ARRAY && code == 44 && array_len(value) == 13)
	{
		Value	   *out = value_object();
		Value	   *opts = value_object();
		Value	   *timing = value_object();

		object_set(out, "Functions", value_int(list_i64(value, 0)));
		object_set(out, "Flags", value_int(list_i64(value, 1)));
		object_set(opts, "Inlining", value_bool(list_bool(value, 2)));
		object_set(opts, "Optimization", value_bool(list_bool(value, 3)));
		object_set(opts, "Expressions", value_bool(list_bool(value, 4)));
		object_set(opts, "Deforming", value_bool(list_bool(value, 5)));
		object_set(out, "Options", opts);
		object_set(timing, "Available", value_bool(list_bool(value, 6)));
		object_set(timing, "Generation", value_double(list_i64(value, 7) / 1000.0));
		object_set(timing, "Deform", value_double(list_i64(value, 8) / 1000.0));
		object_set(timing, "Inlining", value_double(list_i64(value, 9) / 1000.0));
		object_set(timing, "Optimization", value_double(list_i64(value, 10) / 1000.0));
		object_set(timing, "Emission", value_double(list_i64(value, 11) / 1000.0));
		object_set(timing, "Total", value_double(list_i64(value, 12) / 1000.0));
		object_set(out, "Timing", timing);
		return out;
	}
	if (value && value->type == V_ARRAY && code == 47 && array_len(value) == 16)
	{
		static const char *names[] = {
			"Shared Hit Blocks", "Shared Read Blocks", "Shared Dirtied Blocks",
			"Shared Written Blocks", "Local Hit Blocks", "Local Read Blocks",
			"Local Dirtied Blocks", "Local Written Blocks", "Temp Read Blocks",
			"Temp Written Blocks", "Shared Read Time", "Shared Write Time",
			"Local Read Time", "Local Write Time", "Temp Read Time",
			"Temp Write Time",
		};
		Value	   *out = value_object();

		for (int i = 0; i < 10; i++)
			object_set(out, names[i], value_int(list_i64(value, i)));
		for (int i = 10; i < 16; i++)
			object_set(out, names[i], value_double(list_i64(value, i) / 1000.0));
		return out;
	}
	return value_copy(value);
}

static Value *
apply_details(Value *node)
{
	Value	   *raw = object_remove(node, "Details");

	if (raw && raw->type == V_ARRAY)
	{
		for (size_t i = 0; i < raw->v.a->len; i++)
		{
			Value	   *detail = raw->v.a->items[i];
			const char *label = value_cstr(object_get(detail, "label"));

			if (label)
				object_set_append(node, label, detail_to_property(detail));
		}
	}
	{
		Value	   *plans = object_get(node, "Plans");

		if (plans && plans->type == V_ARRAY)
		{
			for (size_t i = 0; i < plans->v.a->len; i++)
				apply_details(plans->v.a->items[i]);
		}
	}
	return node;
}

static TemplateEntry *
template_find(TemplateStore *store, uint64_t id)
{
	for (size_t i = 0; i < store->len; i++)
		if (store->entries[i].id == id)
			return &store->entries[i];
	return NULL;
}

static void
template_store(TemplateStore *store, uint64_t id, uint64_t query_id,
			   uint64_t shape_hash, Value *plan)
{
	TemplateEntry *entry = template_find(store, id);

	if (entry == NULL)
	{
		if (store->len == store->cap)
		{
			store->cap = store->cap ? store->cap * 2 : 64;
			store->entries = xrealloc(store->entries,
									  store->cap * sizeof(TemplateEntry));
		}
		entry = &store->entries[store->len++];
	}
	entry->id = id;
	entry->query_id = query_id;
	entry->shape_hash = shape_hash;
	entry->plan = plan;
}

static Value *
parse_payload(const uint8_t *data, size_t len, Value *record_header,
			  TemplateStore *templates)
{
	Reader		r = {data, len, 0, (int) value_i64(object_get(record_header, "Format Version"), 3)};
	uint32_t	flags = (uint32_t) value_u64(object_get(record_header, "Flags"), 0);
	int64_t		duration_us = (int64_t) (value_double_as(object_get(record_header, "Duration"), 0) * 1000.0);
	uint64_t	query_id = value_u64(object_get(record_header, "Query Identifier"), 0);
	Value	   *context;
	unsigned	template_mode;
	bool		template_has_metrics;
	bool		has_query_details;
	uint64_t	template_id = 0;
	uint64_t	shape_hash = 0;
	uint64_t	plan_id = 0;
	char	   *query_text = NULL;
	char	   *params = NULL;
	Value	   *plan;
	Value	   *query_details = value_array();

	if (r.version >= 4)
	{
		uint8_t		context_ctrl = reader_u8(&r);

		if (r.version >= 6 && (context_ctrl & CONTEXT_REF))
		{
			Value	   *ts = object_get(record_header, "Timestamp");

			if (templates->context == NULL)
				fatal("context referenced before definition");
			context = value_copy(templates->context);
			object_set(context, "Timestamp", value_copy(ts));
		}
		else
		{
			uint8_t		context_ctrl2 = r.version >= 13 ? reader_u8(&r) : 0;
			Value	   *file = object_get(record_header, "File");

			context = value_object();
			object_set(context, "Timestamp", value_copy(object_get(record_header, "Timestamp")));
			object_set(context, "Backend Start", value_copy(object_get(file, "Backend Start")));
			object_set(context, "Backend Start Epoch", value_copy(object_get(file, "Backend Start Epoch")));
			object_set(context, "PID",
					   r.version >= 13 ?
					   value_uint(reader_uint_sized(&r, context_ctrl2 & 0x03)) :
					   value_copy(object_get(file, "PID")));
			object_set(context, "Backend Type ID",
					   value_uint(reader_uint_sized(&r, context_ctrl & 0x03)));
			object_set(context, "Backend Type", value_string_steal(reader_string(&r)));
			object_set(context, "Database OID",
					   value_uint(reader_uint_sized(&r, (context_ctrl >> 2) & 0x03)));
			object_set(context, "Database", value_string_steal(reader_string(&r)));
			object_set(context, "User OID",
					   value_uint(reader_uint_sized(&r, (context_ctrl >> 4) & 0x03)));
			object_set(context, "Authenticated User", value_string_steal(reader_string(&r)));
			object_set(context, "User", value_string_steal(reader_string(&r)));
			object_set(context, "Application Name", value_string_steal(reader_string(&r)));
			object_set(context, "Client Host", value_string_steal(reader_string(&r)));
			object_set(context, "Client Port", value_string_steal(reader_string(&r)));
			if (r.version >= 6)
			{
				templates->context = value_copy(context);
				object_set(templates->context, "Timestamp", value_null());
			}
		}
		{
			uint8_t		template_ctrl = reader_u8(&r);

			template_mode = template_ctrl & 0x03;
			template_has_metrics =
				template_mode == TEMPLATE_REF &&
				(r.version < 5 || (template_ctrl & TEMPLATE_HAS_METRICS));
			has_query_details = r.version < 5 || (template_ctrl & TEMPLATE_HAS_DETAILS);
			if (template_mode != TEMPLATE_NONE)
			{
				template_id = reader_uint_sized(&r, (template_ctrl >> 2) & 0x03);
				if (r.version >= 8 ||
					r.version < 5 || template_mode == TEMPLATE_DEFINE)
				{
					shape_hash = reader_uint_sized(&r, (template_ctrl >> 4) & 0x03);
					plan_id = shape_hash;
				}
			}
			else if (r.version >= 8)
			{
				shape_hash = reader_uint_sized(&r, (template_ctrl >> 4) & 0x03);
				plan_id = shape_hash;
			}
		}
		if (flags & FLAG_QUERY_TEXT)
			query_text = reader_string(&r);
		if (flags & FLAG_PARAMS)
			params = reader_string(&r);
	}
	else
	{
		if (reader_u32(&r) != PAYLOAD_MAGIC)
			fatal("bad payload magic");
		r.version = reader_u16(&r);
		(void) reader_u16(&r);
		flags = reader_u32(&r);
		duration_us = reader_i64(&r);
		query_id = reader_u64(&r);
		context = value_object();
		object_set(context, "Timestamp", value_string_steal(pg_timestamp_to_iso(reader_i64(&r))));
		object_set(context, "Backend Start", value_string_steal(pg_timestamp_to_iso(reader_i64(&r))));
		object_set(context, "Backend Start Epoch", value_int(reader_i64(&r)));
		object_set(context, "PID", value_uint(reader_u32(&r)));
		object_set(context, "Backend Type ID", value_uint(reader_u32(&r)));
		object_set(context, "Backend Type", value_string_steal(reader_string(&r)));
		object_set(context, "Database OID", value_uint(reader_u32(&r)));
		object_set(context, "Database", value_string_steal(reader_string(&r)));
		object_set(context, "User OID", value_uint(reader_u32(&r)));
		object_set(context, "Authenticated User", value_string_steal(reader_string(&r)));
		object_set(context, "User", value_string_steal(reader_string(&r)));
		object_set(context, "Application Name", value_string_steal(reader_string(&r)));
		object_set(context, "Client Host", value_string_steal(reader_string(&r)));
		object_set(context, "Client Port", value_string_steal(reader_string(&r)));
		template_mode = reader_u8(&r);
		template_has_metrics = template_mode == TEMPLATE_REF;
		has_query_details = true;
		template_id = reader_u32(&r);
		shape_hash = reader_u64(&r);
		plan_id = shape_hash;
		query_text = reader_string(&r);
		params = reader_string(&r);
	}

	if (template_mode == TEMPLATE_REF)
	{
		TemplateEntry *entry = template_find(templates, template_id);

		if (entry == NULL)
			fatal("template id %" PRIu64 " referenced before definition", template_id);
		if (shape_hash == 0)
			shape_hash = entry->shape_hash;
		if (plan_id == 0)
			plan_id = shape_hash;
		if (query_id == 0)
		{
			query_id = entry->query_id;
			object_set(record_header, "Query Identifier", value_uint(query_id));
		}
		if (template_has_metrics)
		{
			Value	   *metrics = parse_metric_node(&r);

			plan = merge_template_metrics(entry->plan, metrics);
		}
		else
			plan = value_copy(entry->plan);
	}
	else
	{
		plan = parse_full_plan_node(&r);
		if (template_mode == TEMPLATE_DEFINE)
			template_store(templates, template_id, query_id, shape_hash,
						   strip_dynamic_details(plan));
	}

	if (r.version >= 5)
	{
		if (has_query_details)
			query_details = parse_details(&r);
	}
	else if (r.version >= 3 && r.pos < r.len)
		query_details = parse_details(&r);
	if (r.pos != r.len)
		fatal("%zu trailing payload bytes", r.len - r.pos);

	{
		Value	   *out = value_object();
		Value	   *tmpl = value_object();
		const char *mode_name = template_mode_name(template_mode);

		object_set(out, "Record", record_header);
		object_set(out, "Format Version", value_int(r.version));
		object_set(out, "Flags", value_uint(flags));
		object_set(out, "Duration", value_double(duration_us / 1000.0));
		object_set(out, "Query Identifier", value_uint(query_id));
		object_set(out, "Plan Identifier", value_uint(plan_id));
		object_set(out, "Log Context", context);
		object_set(tmpl, "Mode", mode_name ? value_string(mode_name) :
				   value_string_steal(xasprintf("%u", template_mode)));
		object_set(tmpl, "ID", value_uint(template_id));
		object_set(tmpl, "Shape Hash", value_uint(shape_hash));
		if (template_mode == TEMPLATE_REF)
			object_set(tmpl, "Metrics",
					   value_string(template_has_metrics ? "inline" : "omitted"));
		object_set(out, "Template", tmpl);
		object_set(out, "Plan", apply_details(plan));
		if (query_text != NULL)
			object_set(out, "Query Text", value_string_steal(query_text));
		if (params != NULL)
			object_set(out, "Query Parameters", value_string_steal(params));
		for (size_t i = 0; i < array_len(query_details); i++)
		{
			Value	   *detail = array_get(query_details, i);
			const char *label = value_cstr(object_get(detail, "label"));

			if (label)
				object_set_append(out, label, detail_to_property(detail));
		}
		return out;
	}
}

static void
parse_file_records(const char *path, Value *records)
{
	Binary		file = read_file(path);
	Reader		r = {file.data, file.len, 0, 3};
	uint16_t	file_version;
	uint16_t	header_len;
	TemplateStore templates;

	memset(&templates, 0, sizeof(templates));
	if (reader_u32(&r) != FILE_MAGIC)
		fatal("%s: bad file magic", path);
	file_version = reader_u16(&r);
	header_len = reader_u16(&r);
	if (file_version >= 4)
	{
		uint16_t	file_compression = reader_u16(&r);
		uint16_t	file_flags = reader_u16(&r);
		uint32_t	pg_version = reader_u32(&r);
		int64_t		backend_start = reader_i64(&r);
		int64_t		backend_start_epoch = reader_i64(&r);
		uint32_t	pid = reader_u32(&r);
		uint32_t	reserved = reader_u32(&r);
		Value	   *file_header = value_object();
		Binary		body;
		Reader		br;
		uint64_t	record_index = 0;
		const char *comp = compression_name(file_compression);

		if (verify_crc)
			fatal("--verify-crc is only available for v3 records");
		object_set(file_header, "Path", value_string(path));
		object_set(file_header, "Format Version", value_int(file_version));
		object_set(file_header, "Compression", comp ? value_string(comp) :
				   value_string_steal(xasprintf("%u", file_compression)));
		object_set(file_header, "File Flags", value_int(file_flags));
		object_set(file_header, "PostgreSQL Version", value_uint(pg_version));
		object_set(file_header, "Backend Start",
				   value_string_steal(pg_timestamp_to_iso(backend_start)));
		object_set(file_header, "Backend Start Epoch", value_int(backend_start_epoch));
		object_set(file_header, "PID", value_uint(pid));
		object_set(file_header, "Reserved", value_uint(reserved));
		if (header_len > 40)
			reader_take(&r, header_len - 40);
		body = decompress_file_body(file_compression, r.data + r.pos, r.len - r.pos);
		br.data = body.data;
		br.len = body.len;
		br.pos = 0;
		br.version = file_version;
		while (br.pos < br.len)
		{
			size_t		start = br.pos;
			uint8_t		ctrl1 = reader_u8(&br);
			uint64_t	body_len = reader_uint_sized(&br, ctrl1 & 0x03);
			Binary		record_bin = reader_take(&br, (size_t) body_len);
			Reader		rr = {record_bin.data, record_bin.len, 0, file_version};
			uint8_t		ctrl2 = reader_u8(&rr);
			uint64_t	record_no;
			int64_t		ts_delta;
			int64_t		duration_us;
			uint32_t	flags;
			unsigned	qid_code;
			unsigned	profile;
			uint64_t	query_id;
			Value	   *header = value_object();
			Binary		payload;
			const char *profname;

			record_index++;
			record_no = file_version >= 5 ?
				record_index : reader_uint_sized(&rr, (ctrl1 >> 2) & 0x03);
			ts_delta = reader_int_sized(&rr, (ctrl1 >> 4) & 0x03);
			duration_us = reader_int_sized(&rr, (ctrl1 >> 6) & 0x03);
			flags = (uint32_t) reader_uint_sized(&rr, ctrl2 & 0x03);
			qid_code = (ctrl2 >> 2) & 0x03;
			profile = (ctrl2 >> 4) & 0x01;
			query_id = (ctrl2 & 0x20) ? reader_uint_sized(&rr, qid_code) : 0;
			payload = reader_take(&rr, rr.len - rr.pos);
			profname = profile_name(profile);

			object_set(header, "File", value_copy(file_header));
			object_set(header, "Offset", value_uint(start));
			object_set(header, "Format Version", value_int(file_version));
			object_set(header, "Compression", comp ? value_string(comp) :
					   value_string_steal(xasprintf("%u", file_compression)));
			object_set(header, "Profile", profname ? value_string(profname) :
					   value_string_steal(xasprintf("%u", profile)));
			object_set(header, "Flags", value_uint(flags));
			object_set(header, "Record Number", value_uint(record_no));
			object_set(header, "Timestamp",
					   value_string_steal(pg_timestamp_to_iso(
						   backend_start + (file_version >= 5 ? ts_delta * 1000 : ts_delta))));
			object_set(header, "Query Identifier", value_uint(query_id));
			object_set(header, "Duration", value_double(duration_us / 1000.0));
			if (rr.pos != rr.len)
				fatal("%s: internal record length mismatch", path);
			array_append(records, parse_payload(payload.data, payload.len, header, &templates));
		}
		return;
	}

	{
		Value	   *file_header = value_object();

		object_set(file_header, "Path", value_string(path));
		object_set(file_header, "Format Version", value_int(file_version));
		object_set(file_header, "PostgreSQL Version", value_uint(reader_u32(&r)));
		object_set(file_header, "Backend Start",
				   value_string_steal(pg_timestamp_to_iso(reader_i64(&r))));
		object_set(file_header, "Reserved", value_uint(reader_u32(&r)));
		if (header_len > 24)
			reader_take(&r, header_len - 24);
		while (r.pos < r.len)
		{
			size_t		start = r.pos;
			uint16_t	version;
			uint16_t	rec_header_len;
			uint16_t	compression;
			uint16_t	profile;
			uint32_t	flags;
			uint32_t	expected_crc;
			Value	   *header = value_object();
			uint32_t	uncompressed_len;
			uint32_t	compressed_len;
			Binary		payload_data;
			Binary		payload;
			const char *comp;
			const char *profname;

			if (reader_u32(&r) != RECORD_MAGIC)
				fatal("%s: bad record magic at offset %zu", path, start);
			version = reader_u16(&r);
			rec_header_len = reader_u16(&r);
			compression = reader_u16(&r);
			profile = reader_u16(&r);
			flags = reader_u32(&r);
			comp = compression_name(compression);
			profname = profile_name(profile);
			object_set(header, "File", value_copy(file_header));
			object_set(header, "Offset", value_uint(start));
			object_set(header, "Format Version", value_int(version));
			object_set(header, "Compression", comp ? value_string(comp) :
					   value_string_steal(xasprintf("%u", compression)));
			object_set(header, "Profile", profname ? value_string(profname) :
					   value_string_steal(xasprintf("%u", profile)));
			object_set(header, "Flags", value_uint(flags));
			object_set(header, "Record Number", value_uint(reader_u64(&r)));
			object_set(header, "Timestamp",
					   value_string_steal(pg_timestamp_to_iso(reader_i64(&r))));
			object_set(header, "PID", value_uint(reader_u32(&r)));
			object_set(header, "Database OID", value_uint(reader_u32(&r)));
			object_set(header, "User OID", value_uint(reader_u32(&r)));
			uncompressed_len = reader_u32(&r);
			compressed_len = reader_u32(&r);
			object_set(header, "Uncompressed Length", value_uint(uncompressed_len));
			object_set(header, "Compressed Length", value_uint(compressed_len));
			expected_crc = reader_u32(&r);
			object_set(header, "CRC32C", value_uint(expected_crc));
			object_set(header, "Query Identifier", value_uint(reader_u64(&r)));
			object_set(header, "Duration", value_double(reader_i64(&r) / 1000.0));
			if (rec_header_len > r.pos - start)
				reader_take(&r, rec_header_len - (r.pos - start));
			payload_data = reader_take(&r, compressed_len);
			if (object_get(header, "Format Version"))
				((void) 0);
			if (templates.context)
				((void) 0);
			if (false)
				((void) 0);
			payload = decompress_payload(compression, payload_data.data,
										 payload_data.len, uncompressed_len);
			if (payload.len != uncompressed_len)
				fatal("%s: decompressed length mismatch", path);
			if (verify_crc)
			{
				uint32_t	actual_crc = crc32c_sw(payload.data, payload.len);

				if (actual_crc != expected_crc)
					fatal("%s: CRC32C mismatch", path);
			}
			if (templates.context)
				((void) 0);
			array_append(records, parse_payload(payload.data, payload.len, header, &templates));
		}
	}
}

static const char *
cmd_name(int64_t command)
{
	switch (command)
	{
		case 1: return "Select";
		case 2: return "Update";
		case 3: return "Insert";
		case 4: return "Delete";
		case 5: return "Merge";
		default: return "???";
	}
}

static const char *
join_name(int64_t jointype)
{
	switch (jointype)
	{
		case 0: return "Inner";
		case 1: return "Left";
		case 2: return "Full";
		case 3: return "Right";
		case 4: return "Semi";
		case 5: return "Anti";
		case 6: return "Right Semi";
		case 7: return "Right Anti";
		default: return "???";
	}
}

static const char *
scan_direction_name(int64_t encoded)
{
	switch (encoded - 1)
	{
		case -1: return "Backward";
		case 0: return "NoMovement";
		case 1: return "Forward";
		default: return "???";
	}
}

static bool
extra_value(const Value *node, int kind, int index, int64_t *out)
{
	if (value_i64(object_get(node, "Extra Kind"), 0) == kind)
	{
		*out = value_i64(object_get(node, index == 1 ? "Extra1" : "Extra2"), 0);
		return true;
	}
	return false;
}

static const char *
scan_direction(const Value *node)
{
	int64_t		encoded;

	if (!extra_value(node, EXTRA_INDEX_SCAN_DIRECTION, 1, &encoded))
		return "Forward";
	return scan_direction_name(encoded);
}

static const char *
agg_partial_mode(int64_t aggsplit)
{
	if (aggsplit & 0x02)
		return "Partial";
	if (aggsplit & 0x01)
		return "Finalize";
	return "Simple";
}

static void
postgres_node_names(const Value *node, char **json_name, char **text_name,
					Value **props)
{
	const char *ntype = value_cstr(object_get(node, "Node Type"));
	int64_t		v1;
	int64_t		v2;

	if (ntype == NULL)
		ntype = "???";
	*json_name = xstrdup(ntype);
	*text_name = xstrdup(ntype);
	*props = value_object();

	if (strcmp(ntype, "ModifyTable") == 0)
	{
		const char *operation;

		(void) extra_value(node, EXTRA_MODIFY_OPERATION, 1, &v1);
		operation = cmd_name(v1);
		object_set(*props, "Operation", value_string(operation));
		free(*text_name);
		*text_name = xstrdup(operation);
	}
	else if (strcmp(ntype, "Foreign Scan") == 0)
	{
		const char *operation;

		(void) extra_value(node, EXTRA_FOREIGN_OPERATION, 1, &v1);
		operation = cmd_name(v1);
		object_set(*props, "Operation", value_string(operation));
		if (strcmp(operation, "Select") != 0)
		{
			free(*text_name);
			*text_name = xasprintf("Foreign %s", operation);
		}
	}
	else if (strcmp(ntype, "Nested Loop") == 0 ||
			 strcmp(ntype, "Merge Join") == 0 ||
			 strcmp(ntype, "Hash Join") == 0)
	{
		const char *jointype;

		(void) extra_value(node, EXTRA_JOIN_TYPE, 1, &v1);
		jointype = join_name(v1);
		object_set(*props, "Join Type", value_string(jointype));
		free(*text_name);
		if (strcmp(ntype, "Nested Loop") == 0)
			*text_name = strcmp(jointype, "Inner") == 0 ?
				xstrdup("Nested Loop") : xasprintf("Nested Loop %s Join", jointype);
		else if (strcmp(ntype, "Merge Join") == 0)
			*text_name = strcmp(jointype, "Inner") == 0 ?
				xstrdup("Merge Join") : xasprintf("Merge %s Join", jointype);
		else
			*text_name = strcmp(jointype, "Inner") == 0 ?
				xstrdup("Hash Join") : xasprintf("Hash %s Join", jointype);
	}
	else if (strcmp(ntype, "Aggregate") == 0)
	{
		const char *name = "Aggregate ???";
		const char *strategy = "???";
		const char *partial;

		(void) extra_value(node, EXTRA_AGG, 1, &v1);
		(void) extra_value(node, EXTRA_AGG, 2, &v2);
		switch (v1)
		{
			case 0: name = "Aggregate"; strategy = "Plain"; break;
			case 1: name = "GroupAggregate"; strategy = "Sorted"; break;
			case 2: name = "HashAggregate"; strategy = "Hashed"; break;
			case 3: name = "MixedAggregate"; strategy = "Mixed"; break;
		}
		free(*text_name);
		*text_name = xstrdup(name);
		object_set(*props, "Strategy", value_string(strategy));
		partial = agg_partial_mode(v2);
		if (partial)
		{
			object_set(*props, "Partial Mode", value_string(partial));
			if (strcmp(partial, "Partial") == 0 ||
				strcmp(partial, "Finalize") == 0)
			{
				char	   *old = *text_name;

				*text_name = xasprintf("%s %s", partial, old);
				free(old);
			}
		}
	}
	else if (strcmp(ntype, "SetOp") == 0)
	{
		const char *name = "SetOp ???";
		const char *strategy = "???";
		const char *command = "???";

		(void) extra_value(node, EXTRA_SETOP, 1, &v1);
		(void) extra_value(node, EXTRA_SETOP, 2, &v2);
		switch (v1)
		{
			case 0: name = "SetOp"; strategy = "Sorted"; break;
			case 1: name = "HashSetOp"; strategy = "Hashed"; break;
		}
		switch (v2)
		{
			case 0: command = "Intersect"; break;
			case 1: command = "Intersect All"; break;
			case 2: command = "Except"; break;
			case 3: command = "Except All"; break;
		}
		free(*text_name);
		*text_name = xasprintf("%s %s", name, command);
		object_set(*props, "Strategy", value_string(strategy));
		object_set(*props, "Command", value_string(command));
	}
}

static bool
is_reserved_identifier(const char *name)
{
	static const char *reserved[] = {
		"all", "analyse", "analyze", "and", "any", "array", "as", "asc",
		"asymmetric", "both", "case", "cast", "check", "collate", "column",
		"constraint", "create", "current_catalog", "current_date",
		"current_role", "current_time", "current_timestamp", "current_user",
		"default", "deferrable", "desc", "distinct", "do", "else", "end",
		"except", "false", "fetch", "for", "foreign", "from", "grant",
		"group", "having", "in", "initially", "intersect", "into", "lateral",
		"leading", "limit", "localtime", "localtimestamp", "not", "null",
		"offset", "on", "only", "or", "order", "placing", "primary",
		"references", "returning", "select", "session_user", "some",
		"symmetric", "table", "then", "to", "trailing", "true", "union",
		"unique", "user", "using", "variadic", "when", "where", "window",
		"with", "xmltable", "json_table",
	};
	char	   *lower = xstrdup(name);
	bool		found = false;

	for (char *p = lower; *p; p++)
		*p = (char) tolower((unsigned char) *p);
	for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
	{
		if (strcmp(lower, reserved[i]) == 0)
		{
			found = true;
			break;
		}
	}
	free(lower);
	return found;
}

static char *
quote_identifier(const char *name, bool force)
{
	bool		quote = force;
	Buf			buf;

	if (name == NULL)
		return xstrdup("");
	if (!quote)
	{
		if (!(name[0] == '_' || islower((unsigned char) name[0])))
			quote = true;
		for (const char *p = name; *p && !quote; p++)
		{
			if (!(*p == '_' || islower((unsigned char) *p) ||
				  isdigit((unsigned char) *p)))
				quote = true;
		}
		if (!quote && is_reserved_identifier(name))
			quote = true;
	}
	if (!quote)
		return xstrdup(name);
	buf_init(&buf);
	buf_appendc(&buf, '"');
	for (const char *p = name; *p; p++)
	{
		if (*p == '"')
			buf_appendc(&buf, '"');
		buf_appendc(&buf, *p);
	}
	buf_appendc(&buf, '"');
	return buf_steal(&buf);
}

static void
add_scan_identity(Value *out, const Value *node)
{
	const char *ntype = value_cstr(object_get(node, "Node Type"));
	const char *index_name = value_cstr(object_get(node, "Index Name"));
	const char *relname = value_cstr(object_get(node, "Relation Name"));
	const char *schema = value_cstr(object_get(node, "Schema"));
	const char *alias = value_cstr(object_get(node, "Alias"));

	if (ntype && (strcmp(ntype, "Index Scan") == 0 ||
				  strcmp(ntype, "Index Only Scan") == 0))
		object_set(out, "Scan Direction", value_string(scan_direction(node)));
	if (index_name)
		object_set(out, "Index Name", value_string(index_name));
	if (relname)
	{
		object_set(out, "Relation Name", value_string(relname));
		if (schema)
			object_set(out, "Schema", value_string(schema));
		if (alias)
			object_set(out, "Alias", value_string(alias));
	}
	else if (ntype && strcmp(ntype, "Function Scan") == 0)
	{
		if (value_cstr(object_get(node, "Function Name")))
			object_set(out, "Function Name",
					   value_string(value_cstr(object_get(node, "Function Name"))));
		if (schema)
			object_set(out, "Schema", value_string(schema));
		if (alias)
			object_set(out, "Alias", value_string(alias));
	}
	else if (ntype && strcmp(ntype, "Table Function Scan") == 0)
	{
		if (value_cstr(object_get(node, "Table Function Name")))
			object_set(out, "Table Function Name",
					   value_string(value_cstr(object_get(node, "Table Function Name"))));
		if (alias)
			object_set(out, "Alias", value_string(alias));
	}
	else if (ntype && (strcmp(ntype, "CTE Scan") == 0 ||
					   strcmp(ntype, "WorkTable Scan") == 0))
	{
		if (value_cstr(object_get(node, "CTE Name")))
			object_set(out, "CTE Name",
					   value_string(value_cstr(object_get(node, "CTE Name"))));
		if (alias)
			object_set(out, "Alias", value_string(alias));
	}
	else if (ntype && strcmp(ntype, "Named Tuplestore Scan") == 0)
	{
		if (value_cstr(object_get(node, "Tuplestore Name")))
			object_set(out, "Tuplestore Name",
					   value_string(value_cstr(object_get(node, "Tuplestore Name"))));
		if (alias)
			object_set(out, "Alias", value_string(alias));
	}
	else if (ntype && (strcmp(ntype, "Subquery Scan") == 0 ||
					   strcmp(ntype, "Values Scan") == 0) && alias)
		object_set(out, "Alias", value_string(alias));
}

static void
add_buffer_properties(Value *out, const Value *buffers, bool include_io_timing)
{
	static const char *order[] = {
		"Shared Hit Blocks", "Shared Read Blocks", "Shared Dirtied Blocks",
		"Shared Written Blocks", "Local Hit Blocks", "Local Read Blocks",
		"Local Dirtied Blocks", "Local Written Blocks", "Temp Read Blocks",
		"Temp Written Blocks",
	};
	static const char *timing_src[] = {
		"Shared Read Time", "Shared Write Time", "Local Read Time",
		"Local Write Time", "Temp Read Time", "Temp Write Time",
	};
	static const char *timing_dst[] = {
		"Shared I/O Read Time", "Shared I/O Write Time",
		"Local I/O Read Time", "Local I/O Write Time",
		"Temp I/O Read Time", "Temp I/O Write Time",
	};

	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
		object_set(out, order[i], value_int(value_i64(object_get(buffers, order[i]), 0)));
	if (include_io_timing)
	{
		for (size_t i = 0; i < sizeof(timing_src) / sizeof(timing_src[0]); i++)
			object_set(out, timing_dst[i],
					   value_numeric(numeric_fixed(value_double_as(object_get(buffers, timing_src[i]), 0), 3)));
	}
}

static void
add_wal_properties(Value *out, const Value *wal)
{
	static const char *order[] = {
		"WAL Records", "WAL FPI", "WAL Bytes", "WAL Buffers Full",
	};

	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
		object_set(out, order[i], value_int(value_i64(object_get(wal, order[i]), 0)));
}

static bool
pg_float0_key(const char *key)
{
	static const char *keys[] = {
		"Actual Loops", "Rows Removed by Filter",
		"Rows Removed by Join Filter", "Rows Removed by Index Recheck",
		"Rows Removed by Conflict Filter", "Heap Fetches",
		"Workers Planned", "Workers Launched", "Tuples Inserted",
		"Conflicting Tuples", "Tuples Updated", "Tuples Deleted",
		"Tuples Skipped",
	};

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (strcmp(key, keys[i]) == 0)
			return true;
	return false;
}

static Value *
postgres_detail_value(const char *key, const Value *value)
{
	if (pg_float0_key(key))
	{
		if (value && value->type == V_ARRAY)
		{
			Value	   *out = value_array();

			for (size_t i = 0; i < value->v.a->len; i++)
				array_append(out, postgres_detail_value(key, value->v.a->items[i]));
			return out;
		}
		return value_numeric(numeric_fixed(value_double_as(value, 0), 0));
	}
	return value_copy(value);
}

static Value *
strip_worker_field(const Value *value, const char **fields, size_t nfields)
{
	Value	   *out = value_object();

	if (value == NULL || value->type != V_OBJECT)
		return out;
	if (fields != NULL)
	{
		for (size_t i = 0; i < nfields; i++)
		{
			Value	   *field = object_get(value, fields[i]);

			if (field)
				object_set(out, fields[i], value_copy(field));
		}
		return out;
	}
	for (size_t i = 0; i < value->v.o->len; i++)
	{
		const char *key = value->v.o->pairs[i].key;
		bool		keep = strcmp(key, "Worker") != 0;

		if (keep)
			object_set(out, key, value_copy(value->v.o->pairs[i].value));
	}
	return out;
}

static void
object_merge(Value *dst, const Value *src)
{
	if (src == NULL || src->type != V_OBJECT)
		return;
	for (size_t i = 0; i < src->v.o->len; i++)
		object_set(dst, src->v.o->pairs[i].key, value_copy(src->v.o->pairs[i].value));
}

static void
add_worker(Value *out, int64_t worker, const Value *props)
{
	Value	   *workers = object_get(out, "Workers");

	if (workers == NULL)
	{
		workers = value_array();
		object_set(out, "Workers", workers);
	}
	for (size_t i = 0; i < array_len(workers); i++)
	{
		Value	   *entry = array_get(workers, i);

		if (value_i64(object_get(entry, "Worker Number"), -1) == worker)
		{
			object_merge(entry, props);
			return;
		}
	}
	{
		Value	   *entry = value_object();

		object_set(entry, "Worker Number", value_int(worker));
		object_merge(entry, props);
		array_append(workers, entry);
	}
}

static void
add_worker_or_top_level(Value *out, const Value *values,
						const char **fields, size_t nfields)
{
	Value	   *tmp = NULL;
	const Value *array = values;

	if (values == NULL)
		return;
	if (values->type != V_ARRAY)
	{
		tmp = value_array();
		array_append(tmp, value_copy(values));
		array = tmp;
	}
	for (size_t i = 0; i < array_len(array); i++)
	{
		Value	   *value = array_get(array, i);
		Value	   *props;
		int64_t		worker;

		if (value == NULL || value->type != V_OBJECT || !object_has(value, "Worker"))
		{
			if (value && value->type == V_OBJECT)
				object_merge(out, value);
			continue;
		}
		worker = value_i64(object_get(value, "Worker"), -1);
		props = strip_worker_field(value, fields, nfields);
		if (worker < 0)
			object_merge(out, props);
		else
			add_worker(out, worker, props);
	}
}

static void
add_incremental_sort_groups(Value *out, const Value *values)
{
	Value	   *tmp = NULL;
	const Value *array = values;

	if (values == NULL)
		return;
	if (values->type != V_ARRAY)
	{
		tmp = value_array();
		array_append(tmp, value_copy(values));
		array = tmp;
	}
	for (size_t i = 0; i < array_len(array); i++)
	{
		Value	   *value = array_get(array, i);
		Value	   *group;
		Value	   *space;
		int64_t		worker;
		const char *group_type;
		char	   *label;

		if (value == NULL || value->type != V_OBJECT)
			continue;
		worker = value_i64(object_get(value, "Worker"), -1);
		group_type = value_cstr(object_get(value, "Group Type"));
		if (group_type == NULL || group_type[0] == '\0')
			continue;
		group = value_object();
		object_set(group, "Group Count",
				   value_int(value_i64(object_get(value, "Group Count"), 0)));
		object_set(group, "Sort Methods Used",
				   value_copy(object_get(value, "Sort Methods Used")));
		if (value_i64(object_get(value, "Peak Memory"), 0) > 0)
		{
			const char *space_type = value_cstr(object_get(value, "Memory Space Type"));
			char	   *space_label;

			if (space_type == NULL)
				space_type = "Memory";
			space = value_object();
			object_set(space, "Average Sort Space Used",
					   value_int(value_i64(object_get(value, "Average Memory"), 0)));
			object_set(space, "Peak Sort Space Used",
					   value_int(value_i64(object_get(value, "Peak Memory"), 0)));
			space_label = xasprintf("Sort Space %s", space_type);
			object_set(group, space_label, space);
		}
		if (value_i64(object_get(value, "Peak Disk"), 0) > 0)
		{
			space = value_object();
			object_set(space, "Average Sort Space Used",
					   value_int(value_i64(object_get(value, "Average Disk"), 0)));
			object_set(space, "Peak Sort Space Used",
					   value_int(value_i64(object_get(value, "Peak Disk"), 0)));
			object_set(group, "Sort Space Disk", space);
		}
		label = xasprintf("%s Groups", group_type);
		if (worker < 0)
			object_set(out, label, group);
		else
		{
			Value	   *props = value_object();

			object_set(props, label, group);
			add_worker(out, worker, props);
		}
	}
}

static void
add_postgres_property(Value *out, const char *key, Value *value)
{
	object_set_append(out, key, value);
}

static bool
extension_explain_properties(Value *out, const char *text)
{
	const char *p = text;

	while (*p)
	{
		const char *line_start = p;
		const char *line_end;
		const char *colon;
		const char *key_start;
		const char *key_end;
		const char *val_start;
		char	   *key;
		char	   *raw;
		size_t		raw_len;
		char	   *endptr;
		long long	bytes;

		line_end = strchr(p, '\n');
		if (line_end == NULL)
			line_end = p + strlen(p);
		colon = memchr(line_start, ':', (size_t) (line_end - line_start));
		if (colon == NULL)
			return false;
		key_start = line_start;
		while (key_start < colon && isspace((unsigned char) *key_start))
			key_start++;
		key_end = colon;
		while (key_end > key_start && isspace((unsigned char) key_end[-1]))
			key_end--;
		val_start = colon + 1;
		while (val_start < line_end && isspace((unsigned char) *val_start))
			val_start++;
		key = xstrndup(key_start, (size_t) (key_end - key_start));
		raw_len = (size_t) (line_end - val_start);
		raw = xstrndup(val_start, raw_len);
		errno = 0;
		bytes = strtoll(raw, &endptr, 10);
		if (errno == 0 && endptr != raw && strcmp(endptr, " b") == 0)
			add_postgres_property(out, key, value_int(bytes));
		else
			add_postgres_property(out, key, value_string(raw));
		p = *line_end ? line_end + 1 : line_end;
	}
	return true;
}

static Value *
postgres_trigger(const Value *trigger, uint32_t record_flags)
{
	Value	   *out = value_object();

	object_set(out, "Trigger Name",
			   value_string(value_cstr(object_get(trigger, "Trigger Name")) ?
							value_cstr(object_get(trigger, "Trigger Name")) : ""));
	if (value_truthy(object_get(trigger, "Constraint Name")))
		object_set(out, "Constraint Name",
				   value_string(value_cstr(object_get(trigger, "Constraint Name"))));
	if (value_truthy(object_get(trigger, "Relation")))
		object_set(out, "Relation",
				   value_string(value_cstr(object_get(trigger, "Relation"))));
	if (record_flags & FLAG_TIMING)
		object_set(out, "Time",
				   value_numeric(numeric_fixed(value_double_as(object_get(trigger, "Time"), 0), 3)));
	object_set(out, "Calls",
			   value_int((int64_t) llround(value_double_as(object_get(trigger, "Calls"), 0))));
	return out;
}

static Value *
postgres_jit(const Value *jit, uint32_t record_flags)
{
	Value	   *out = value_object();
	Value	   *timing = object_get(jit, "Timing");

	object_set(out, "Functions", value_int(value_i64(object_get(jit, "Functions"), 0)));
	object_set(out, "Options", value_copy(object_get(jit, "Options")));
	if ((record_flags & FLAG_ANALYZE) && (record_flags & FLAG_TIMING) &&
		value_truthy(object_get(timing, "Available")))
	{
		Value	   *tout = value_object();
		Value	   *generation = value_object();

		object_set(generation, "Deform",
				   value_numeric(numeric_fixed(value_double_as(object_get(timing, "Deform"), 0), 3)));
		object_set(generation, "Total",
				   value_numeric(numeric_fixed(value_double_as(object_get(timing, "Generation"), 0), 3)));
		object_set(tout, "Generation", generation);
		object_set(tout, "Inlining",
				   value_numeric(numeric_fixed(value_double_as(object_get(timing, "Inlining"), 0), 3)));
		object_set(tout, "Optimization",
				   value_numeric(numeric_fixed(value_double_as(object_get(timing, "Optimization"), 0), 3)));
		object_set(tout, "Emission",
				   value_numeric(numeric_fixed(value_double_as(object_get(timing, "Emission"), 0), 3)));
		object_set(tout, "Total",
				   value_numeric(numeric_fixed(value_double_as(object_get(timing, "Total"), 0), 3)));
		object_set(out, "Timing", tout);
	}
	return out;
}

static bool
skip_plan_key(const char *key)
{
	static const char *skip[] = {
		"Relationship", "Node Tag", "Node Type", "Flags", "Startup Cost",
		"Total Cost", "Plan Rows", "Plan Width", "Startup Time",
		"Total Time", "Actual Rows", "Actual Loops", "Never Executed",
		"Buffers", "WAL", "Relation OID", "Index OID", "Relation Name",
		"Schema", "Alias", "Index Name", "Plans", "Extra Kind", "Extra1",
		"Extra2", "Inner Unique", "Custom Plan Provider", "Function Name",
		"Table Function Name", "CTE Name", "Tuplestore Name", "Subplan Name",
	};

	for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
		if (strcmp(key, skip[i]) == 0)
			return true;
	return false;
}

static Value *
postgres_plan_node(const Value *node, uint32_t record_flags)
{
	char	   *json_name;
	char	   *text_name;
	Value	   *props;
	uint32_t	flags = (uint32_t) value_u64(object_get(node, "Flags"), 0);
	bool		timing = (record_flags & FLAG_TIMING) != 0;
	Value	   *out = value_object();
	Value	   *subplans_removed = NULL;
	Value	   *workers = NULL;
	Value	   *children;

	postgres_node_names(node, &json_name, &text_name, &props);
	object_set(out, "Node Type", value_string(json_name));
	for (size_t i = 0; i < props->v.o->len; i++)
	{
		const char *key = props->v.o->pairs[i].key;

		if (strcmp(key, "Join Type") != 0 && strcmp(key, "Command") != 0)
			object_set(out, key, value_copy(props->v.o->pairs[i].value));
	}
	if (value_truthy(object_get(node, "Relationship")))
		object_set(out, "Parent Relationship", value_copy(object_get(node, "Relationship")));
	if (value_truthy(object_get(node, "Subplan Name")))
		object_set(out, "Subplan Name", value_copy(object_get(node, "Subplan Name")));
	if (object_has(node, "Custom Plan Provider"))
		object_set(out, "Custom Plan Provider",
				   value_copy(object_get(node, "Custom Plan Provider")));
	object_set(out, "Parallel Aware", value_bool((flags & NODE_PARALLEL_AWARE) != 0));
	object_set(out, "Async Capable", value_bool((flags & NODE_ASYNC_CAPABLE) != 0));
	add_scan_identity(out, node);
	for (size_t i = 0; i < props->v.o->len; i++)
	{
		const char *key = props->v.o->pairs[i].key;

		if (strcmp(key, "Join Type") == 0 || strcmp(key, "Command") == 0)
			object_set(out, key, value_copy(props->v.o->pairs[i].value));
	}
	object_set(out, "Startup Cost",
			   value_numeric(numeric_fixed(value_double_as(object_get(node, "Startup Cost"), 0), 2)));
	object_set(out, "Total Cost",
			   value_numeric(numeric_fixed(value_double_as(object_get(node, "Total Cost"), 0), 2)));
	object_set(out, "Plan Rows",
			   value_int((int64_t) llround(value_double_as(object_get(node, "Plan Rows"), 0))));
	object_set(out, "Plan Width", value_int(value_i64(object_get(node, "Plan Width"), 0)));
	if (object_has(node, "Actual Rows") || object_has(node, "Never Executed"))
	{
		if (timing)
		{
			object_set(out, "Actual Startup Time",
					   value_numeric(numeric_fixed(value_double_as(object_get(node, "Startup Time"), 0), 3)));
			object_set(out, "Actual Total Time",
					   value_numeric(numeric_fixed(value_double_as(object_get(node, "Total Time"), 0), 3)));
		}
		object_set(out, "Actual Rows",
				   value_numeric(numeric_fixed(value_double_as(object_get(node, "Actual Rows"), 0), 2)));
		object_set(out, "Actual Loops",
				   value_int((int64_t) llround(value_double_as(object_get(node, "Actual Loops"), 0))));
	}
	object_set(out, "Disabled", value_bool((flags & NODE_DISABLED) != 0));
	if (object_has(node, "Inner Unique"))
		object_set(out, "Inner Unique", value_bool(value_truthy(object_get(node, "Inner Unique"))));

	if (node->type == V_OBJECT)
	{
		for (size_t i = 0; i < node->v.o->len; i++)
		{
			const char *key = node->v.o->pairs[i].key;
			Value	   *value = node->v.o->pairs[i].value;

			if (skip_plan_key(key))
				continue;
			if (strcmp(key, "Sort Info") == 0)
			{
				const char *fields[] = {"Sort Method", "Sort Space Used", "Sort Space Type"};

				add_worker_or_top_level(out, value, fields, 3);
			}
			else if (strcmp(key, "Incremental Sort Groups") == 0)
				add_incremental_sort_groups(out, value);
			else if (strcmp(key, "Hash Info") == 0 ||
					 strcmp(key, "Storage Info") == 0 ||
					 strcmp(key, "Memoize Stats") == 0 ||
					 strcmp(key, "HashAgg Stats") == 0 ||
					 strcmp(key, "Heap Blocks") == 0)
				add_worker_or_top_level(out, value, NULL, 0);
			else if (strcmp(key, "Extension Explain") == 0)
			{
				Value	   *vals = value;
				Value	   *tmp = NULL;

				if (value->type != V_ARRAY)
				{
					tmp = value_array();
					array_append(tmp, value_copy(value));
					vals = tmp;
				}
				for (size_t j = 0; j < array_len(vals); j++)
				{
					const char *text = value_cstr(array_get(vals, j));
					Value	   *parsed = value_object();

					if (text && extension_explain_properties(parsed, text))
						object_merge(out, parsed);
					else
						add_postgres_property(out, key,
											  postgres_detail_value(key, array_get(vals, j)));
				}
			}
			else
				object_set(out, key, postgres_detail_value(key, value));
		}
	}

	subplans_removed = object_remove(out, "Subplans Removed");
	workers = object_remove(out, "Workers");
	if (object_has(node, "Buffers"))
		add_buffer_properties(out, object_get(node, "Buffers"), false);
	if (object_has(node, "WAL"))
		add_wal_properties(out, object_get(node, "WAL"));
	if (subplans_removed)
		object_set(out, "Subplans Removed", subplans_removed);
	if (workers)
		object_set(out, "Workers", workers);
	if ((record_flags & FLAG_ANALYZE) &&
		object_has(node, "Actual Loops") &&
		value_double_as(object_get(node, "Actual Loops"), 0) > 1 &&
		((flags & NODE_PARALLEL_AWARE) ||
		 (object_has(props, "Partial Mode") &&
		  strcmp(value_cstr(object_get(props, "Partial Mode")), "Partial") == 0)))
		object_set(out, "Workers", value_array());

	children = value_array();
	if (object_get(node, "Plans") && object_get(node, "Plans")->type == V_ARRAY)
	{
		Value	   *plans = object_get(node, "Plans");

		for (size_t i = 0; i < plans->v.a->len; i++)
			array_append(children, postgres_plan_node(plans->v.a->items[i], record_flags));
	}
	if (array_len(children) > 0)
		object_set(out, "Plans", children);
	(void) text_name;
	return out;
}

static Value *
postgres_record(const Value *record)
{
	uint32_t	flags = (uint32_t) value_u64(object_get(record, "Flags"), 0);
	Value	   *out = value_object();

	if (value_truthy(object_get(record, "Query Text")))
		object_set(out, "Query Text", value_copy(object_get(record, "Query Text")));
	if (value_truthy(object_get(record, "Query Parameters")))
		object_set(out, "Query Parameters", value_copy(object_get(record, "Query Parameters")));
	object_set(out, "Plan", postgres_plan_node(object_get(record, "Plan"), flags));
	if ((flags & FLAG_VERBOSE) && value_truthy(object_get(record, "Query Identifier")))
		object_set(out, "Query Identifier",
				   value_int(pg_signed_i64(value_u64(object_get(record, "Query Identifier"), 0))));
	if (object_has(record, "Planning"))
	{
		Value	   *planning = value_object();

		add_buffer_properties(planning, object_get(record, "Planning"), false);
		object_set(out, "Planning", planning);
	}
	if (object_has(record, "Trigger"))
	{
		Value	   *triggers = value_array();
		Value	   *raw = object_get(record, "Trigger");

		if (raw->type == V_ARRAY)
		{
			for (size_t i = 0; i < raw->v.a->len; i++)
				array_append(triggers, postgres_trigger(raw->v.a->items[i], flags));
		}
		else
			array_append(triggers, postgres_trigger(raw, flags));
		object_set(out, "Triggers", triggers);
	}
	else if (flags & FLAG_ANALYZE)
		object_set(out, "Triggers", value_array());
	if (object_has(record, "JIT"))
		object_set(out, "JIT", postgres_jit(object_get(record, "JIT"), flags));
	return out;
}

static Value *
postgres_records(const Value *records)
{
	Value	   *out = value_array();

	for (size_t i = 0; i < array_len(records); i++)
		array_append(out, postgres_record(array_get(records, i)));
	return out;
}

static void
json_escape(Buf *buf, const char *s)
{
	buf_appendc(buf, '"');
	for (const unsigned char *p = (const unsigned char *) (s ? s : ""); *p; p++)
	{
		switch (*p)
		{
			case '"': buf_appends(buf, "\\\""); break;
			case '\\': buf_appends(buf, "\\\\"); break;
			case '\b': buf_appends(buf, "\\b"); break;
			case '\f': buf_appends(buf, "\\f"); break;
			case '\n': buf_appends(buf, "\\n"); break;
			case '\r': buf_appends(buf, "\\r"); break;
			case '\t': buf_appends(buf, "\\t"); break;
			default:
				if (*p < 0x20)
					buf_appendf(buf, "\\u%04x", *p);
				else
					buf_appendc(buf, (char) *p);
				break;
		}
	}
	buf_appendc(buf, '"');
}

static char *
double_str(double value)
{
	char		tmp[128];

	if (isnan(value))
		return xstrdup("NaN");
	if (isinf(value))
		return xstrdup(value > 0 ? "Infinity" : "-Infinity");
	snprintf(tmp, sizeof(tmp), "%.15g", value);
	if (strchr(tmp, '.') == NULL && strchr(tmp, 'e') == NULL &&
		strchr(tmp, 'E') == NULL)
		return xasprintf("%s.0", tmp);
	return xstrdup(tmp);
}

static void
render_scalar(Buf *buf, const Value *value, bool quote_strings)
{
	char	   *tmp;

	if (value == NULL || value->type == V_NULL)
	{
		buf_appends(buf, "null");
		return;
	}
	switch (value->type)
	{
		case V_NULL:
			buf_appends(buf, "null");
			break;
		case V_BOOL:
			buf_appends(buf, value->v.b ? "true" : "false");
			break;
		case V_INT:
			buf_appendf(buf, "%" PRId64, value->v.i);
			break;
		case V_UINT:
			buf_appendf(buf, "%" PRIu64, value->v.u);
			break;
		case V_DOUBLE:
			tmp = double_str(value->v.d);
			buf_appends(buf, tmp);
			break;
		case V_NUMERIC:
			buf_appends(buf, value->v.s);
			break;
		case V_STRING:
			if (quote_strings)
				json_escape(buf, value->v.s);
			else
				buf_appends(buf, value->v.s);
			break;
		default:
			buf_appends(buf, "null");
			break;
	}
}

static bool
json_array_multiline(const Value *array, const char *key)
{
	if (array_len(array) == 0)
		return true;
	if (key && (strcmp(key, "Plans") == 0 ||
				strcmp(key, "Triggers") == 0 ||
				strcmp(key, "Workers") == 0))
		return true;
	for (size_t i = 0; i < array_len(array); i++)
	{
		Value	   *item = array_get(array, i);

		if (item && (item->type == V_OBJECT || item->type == V_ARRAY))
			return true;
	}
	return false;
}

static void
render_json_value(Buf *buf, const Value *value, int indent, const char *key)
{
	if (value == NULL)
	{
		buf_appends(buf, "null");
		return;
	}
	if (value->type == V_OBJECT)
	{
		if (value->v.o->len == 0)
		{
			buf_appends(buf, "{}");
			return;
		}
		buf_appends(buf, "{");
		for (size_t i = 0; i < value->v.o->len; i++)
		{
			Pair	   *pair = &value->v.o->pairs[i];

			buf_appendc(buf, '\n');
			for (int j = 0; j < (indent + 1) * 2; j++)
				buf_appendc(buf, ' ');
			json_escape(buf, pair->key);
			buf_appends(buf, ": ");
			render_json_value(buf, pair->value, indent + 1, pair->key);
			if (i + 1 < value->v.o->len)
				buf_appendc(buf, ',');
		}
		buf_appendc(buf, '\n');
		for (int j = 0; j < indent * 2; j++)
			buf_appendc(buf, ' ');
		buf_appendc(buf, '}');
		return;
	}
	if (value->type == V_ARRAY)
	{
		if (json_array_multiline(value, key))
		{
			buf_appends(buf, "[");
			for (size_t i = 0; i < value->v.a->len; i++)
			{
				buf_appendc(buf, '\n');
				for (int j = 0; j < (indent + 1) * 2; j++)
					buf_appendc(buf, ' ');
				render_json_value(buf, value->v.a->items[i], indent + 1, key);
				if (i + 1 < value->v.a->len)
					buf_appendc(buf, ',');
			}
			buf_appendc(buf, '\n');
			for (int j = 0; j < indent * 2; j++)
				buf_appendc(buf, ' ');
			buf_appendc(buf, ']');
		}
		else
		{
			buf_appendc(buf, '[');
			for (size_t i = 0; i < value->v.a->len; i++)
			{
				if (i > 0)
					buf_appends(buf, ", ");
				render_scalar(buf, value->v.a->items[i], true);
			}
			buf_appendc(buf, ']');
		}
		return;
	}
	render_scalar(buf, value, true);
}

static void
render_json_compact(Buf *buf, const Value *value)
{
	if (value == NULL)
	{
		buf_appends(buf, "null");
		return;
	}
	if (value->type == V_OBJECT)
	{
		buf_appendc(buf, '{');
		for (size_t i = 0; i < value->v.o->len; i++)
		{
			if (i > 0)
				buf_appends(buf, ", ");
			json_escape(buf, value->v.o->pairs[i].key);
			buf_appends(buf, ": ");
			render_json_compact(buf, value->v.o->pairs[i].value);
		}
		buf_appendc(buf, '}');
	}
	else if (value->type == V_ARRAY)
	{
		buf_appendc(buf, '[');
		for (size_t i = 0; i < value->v.a->len; i++)
		{
			if (i > 0)
				buf_appends(buf, ", ");
			render_json_compact(buf, value->v.a->items[i]);
		}
		buf_appendc(buf, ']');
	}
	else
		render_scalar(buf, value, true);
}

static char *
json_compact_string(const Value *value)
{
	Buf			buf;

	buf_init(&buf);
	render_json_compact(&buf, value);
	return buf_steal(&buf);
}

static void
yaml_scalar(Buf *buf, const Value *value)
{
	render_scalar(buf, value, value && value->type == V_STRING);
}

static void
yaml_mapping_item(Buf *buf, const char *key, const Value *value, int indent,
				  const char *prefix)
{
	for (int i = 0; i < indent; i++)
		buf_appendc(buf, ' ');
	if (prefix)
		buf_appends(buf, prefix);
	buf_appendf(buf, "%s: ", key);
	if (value && (value->type == V_OBJECT || value->type == V_ARRAY))
	{
		buf_appendc(buf, '\n');
		render_yaml_obj(buf, value, indent + (prefix ? 4 : 2));
	}
	else
	{
		yaml_scalar(buf, value);
		buf_appendc(buf, '\n');
	}
}

static void
render_yaml_dict(Buf *buf, const Value *obj, int indent, const char *first_prefix)
{
	for (size_t i = 0; i < obj->v.o->len; i++)
	{
		const char *prefix = (i == 0 && first_prefix) ? first_prefix : NULL;
		int			key_indent = (prefix || first_prefix == NULL) ? indent : indent + 2;

		yaml_mapping_item(buf, obj->v.o->pairs[i].key, obj->v.o->pairs[i].value,
						  key_indent, prefix);
	}
}

static void
render_yaml_obj(Buf *buf, const Value *value, int indent)
{
	if (value == NULL)
		return;
	if (value->type == V_OBJECT)
		render_yaml_dict(buf, value, indent, NULL);
	else if (value->type == V_ARRAY)
	{
		for (size_t i = 0; i < value->v.a->len; i++)
		{
			Value	   *item = value->v.a->items[i];

			if (item->type == V_OBJECT)
			{
				if (item->v.o->len == 0)
				{
					for (int j = 0; j < indent; j++)
						buf_appendc(buf, ' ');
					buf_appends(buf, "- \n");
				}
				else
					render_yaml_dict(buf, item, indent, "- ");
			}
			else if (item->type == V_ARRAY)
			{
				for (int j = 0; j < indent; j++)
					buf_appendc(buf, ' ');
				buf_appends(buf, "- \n");
				render_yaml_obj(buf, item, indent + 2);
			}
			else
			{
				for (int j = 0; j < indent; j++)
					buf_appendc(buf, ' ');
				buf_appends(buf, "- ");
				yaml_scalar(buf, item);
				buf_appendc(buf, '\n');
			}
		}
	}
	else
	{
		for (int j = 0; j < indent; j++)
			buf_appendc(buf, ' ');
		yaml_scalar(buf, value);
		buf_appendc(buf, '\n');
	}
}

static char *
xml_tag_name(const char *key)
{
	Buf			buf;

	if (strcmp(key, "Full-sort Groups") == 0 ||
		strcmp(key, "Pre-sorted Groups") == 0)
		return xstrdup("Incremental-Sort-Groups");
	if (strcmp(key, "Sort Space Memory") == 0 ||
		strcmp(key, "Sort Space Disk") == 0)
		return xstrdup("Sort-Space");
	buf_init(&buf);
	for (const unsigned char *p = (const unsigned char *) key; *p; p++)
	{
		if (isalnum(*p) || *p == '_' || *p == '.' || *p == '-')
			buf_appendc(&buf, (char) *p);
		else
			buf_appendc(&buf, '-');
	}
	return buf_steal(&buf);
}

static void
xml_scalar(Buf *buf, const Value *value)
{
	char	   *tmp = NULL;
	const char *text = "";

	if (value == NULL || value->type == V_NULL)
		text = "";
	else if (value->type == V_BOOL)
		text = value->v.b ? "true" : "false";
	else if (value->type == V_STRING || value->type == V_NUMERIC)
		text = value->v.s;
	else if (value->type == V_INT)
		text = tmp = xasprintf("%" PRId64, value->v.i);
	else if (value->type == V_UINT)
		text = tmp = xasprintf("%" PRIu64, value->v.u);
	else if (value->type == V_DOUBLE)
		text = tmp = double_str(value->v.d);

	for (const char *p = text; *p; p++)
	{
		switch (*p)
		{
			case '&': buf_appends(buf, "&amp;"); break;
			case '<': buf_appends(buf, "&lt;"); break;
			case '>': buf_appends(buf, "&gt;"); break;
			case '\r': buf_appends(buf, "&#x0d;"); break;
			default: buf_appendc(buf, *p); break;
		}
	}
	(void) tmp;
}

static const char *
xml_item_tag(const char *parent_key)
{
	if (strcmp(parent_key, "Plans") == 0)
		return "Plan";
	if (strcmp(parent_key, "Triggers") == 0)
		return "Trigger";
	if (strcmp(parent_key, "Workers") == 0)
		return "Worker";
	return "Item";
}

static void
render_xml_value(Buf *buf, const char *key, const Value *value, int indent)
{
	char	   *tag = xml_tag_name(key);

	for (int i = 0; i < indent * 2; i++)
		buf_appendc(buf, ' ');
	if (value && value->type == V_OBJECT)
	{
		buf_appendf(buf, "<%s>\n", tag);
		for (size_t i = 0; i < value->v.o->len; i++)
			render_xml_value(buf, value->v.o->pairs[i].key,
							 value->v.o->pairs[i].value, indent + 1);
		for (int i = 0; i < indent * 2; i++)
			buf_appendc(buf, ' ');
		buf_appendf(buf, "</%s>\n", tag);
	}
	else if (value && value->type == V_ARRAY)
	{
		const char *item_tag = xml_item_tag(key);

		buf_appendf(buf, "<%s>\n", tag);
		for (size_t i = 0; i < value->v.a->len; i++)
		{
			Value	   *item = value->v.a->items[i];

			for (int j = 0; j < (indent + 1) * 2; j++)
				buf_appendc(buf, ' ');
			if (item->type == V_OBJECT)
			{
				buf_appendf(buf, "<%s>\n", item_tag);
				for (size_t j = 0; j < item->v.o->len; j++)
					render_xml_value(buf, item->v.o->pairs[j].key,
									 item->v.o->pairs[j].value, indent + 2);
				for (int j = 0; j < (indent + 1) * 2; j++)
					buf_appendc(buf, ' ');
				buf_appendf(buf, "</%s>\n", item_tag);
			}
			else
			{
				buf_appends(buf, "<Item>");
				xml_scalar(buf, item);
				buf_appends(buf, "</Item>\n");
			}
		}
		for (int i = 0; i < indent * 2; i++)
			buf_appendc(buf, ' ');
		buf_appendf(buf, "</%s>\n", tag);
	}
	else
	{
		buf_appendf(buf, "<%s>", tag);
		xml_scalar(buf, value);
		buf_appendf(buf, "</%s>\n", tag);
	}
}

static bool
value_is_string_array(const Value *value)
{
	if (value == NULL || value->type != V_ARRAY)
		return false;
	for (size_t i = 0; i < value->v.a->len; i++)
	{
		Value	   *item = value->v.a->items[i];

		if (item == NULL || item->type != V_STRING)
			return false;
	}
	return true;
}

static char *
join_string_array(const Value *value, const char *sep)
{
	Buf			buf;

	buf_init(&buf);
	for (size_t i = 0; i < array_len(value); i++)
	{
		if (i > 0)
			buf_appends(&buf, sep);
		buf_appends(&buf, value_cstr(array_get(value, i)));
	}
	return buf_steal(&buf);
}

static char *
format_incremental_sort_group_text(const char *label, const Value *value)
{
	Value	   *methods = object_get(value, "Sort Methods Used");
	char	   *methods_joined = join_string_array(methods, ", ");
	const char *method_label = array_len(methods) > 1 ? "Sort Methods" : "Sort Method";
	Buf			buf;

	buf_init(&buf);
	buf_appendf(&buf, "%s: %" PRId64, label,
				value_i64(object_get(value, "Group Count"), 0));
	buf_appendf(&buf, "  %s: %s", method_label, methods_joined);
	for (int i = 0; i < 2; i++)
	{
		const char *key = i == 0 ? "Sort Space Memory" : "Sort Space Disk";
		const char *space_name = i == 0 ? "Memory" : "Disk";
		Value	   *space = object_get(value, key);

		if (space && space->type == V_OBJECT)
		{
			buf_appendf(&buf, "  Average %s: %" PRId64 "kB",
						space_name,
						value_i64(object_get(space, "Average Sort Space Used"), 0));
			buf_appendf(&buf, "  Peak %s: %" PRId64 "kB",
						space_name,
						value_i64(object_get(space, "Peak Sort Space Used"), 0));
		}
	}
	return buf_steal(&buf);
}

static char *
format_memoize_stats_text(const Value *value)
{
	return xasprintf("Hits: %" PRId64 "  Misses: %" PRId64
					 "  Evictions: %" PRId64 "  Overflows: %" PRId64
					 "  Memory Usage: %" PRId64 "kB",
					 value_i64(object_get(value, "Cache Hits"), 0),
					 value_i64(object_get(value, "Cache Misses"), 0),
					 value_i64(object_get(value, "Cache Evictions"), 0),
					 value_i64(object_get(value, "Cache Overflows"), 0),
					 value_i64(object_get(value, "Peak Memory Usage"), 0));
}

static char *
render_value_text(const char *label, const Value *value)
{
	if (strncmp(label, "Rows Removed by", strlen("Rows Removed by")) == 0)
		return xasprintf("%.0f", value_double_as(value, 0));
	if (strcmp(label, "Heap Fetches") == 0 ||
		strcmp(label, "Workers Planned") == 0 ||
		strcmp(label, "Workers Launched") == 0 ||
		strcmp(label, "Index Searches") == 0 ||
		strcmp(label, "Subplans Removed") == 0)
		return xasprintf("%.0f", value_double_as(value, 0));
	if (value && value->type == V_ARRAY)
	{
		if (value_is_string_array(value))
			return join_string_array(value, ", ");
		return json_compact_string(value);
	}
	if (value && value->type == V_OBJECT)
	{
		if (strcmp(label, "Sort Info") == 0)
		{
			const char *prefix = "";

			if (value_i64(object_get(value, "Worker"), -1) >= 0)
				prefix = xasprintf("Worker %" PRId64 ":  ",
								   value_i64(object_get(value, "Worker"), -1));
			return xasprintf("%sSort Method: %s  %s: %" PRId64 "kB",
							 prefix,
							 value_cstr(object_get(value, "Sort Method")),
							 value_cstr(object_get(value, "Sort Space Type")),
							 value_i64(object_get(value, "Sort Space Used"), 0));
		}
		if (strcmp(label, "Hash Info") == 0)
		{
			int64_t		b = value_i64(object_get(value, "Hash Buckets"), 0);
			int64_t		ob = value_i64(object_get(value, "Original Hash Buckets"), 0);
			int64_t		ba = value_i64(object_get(value, "Hash Batches"), 0);
			int64_t		oba = value_i64(object_get(value, "Original Hash Batches"), 0);
			int64_t		mem = value_i64(object_get(value, "Peak Memory Usage"), 0);

			if (b != ob || ba != oba)
				return xasprintf("Buckets: %" PRId64 " (originally %" PRId64
								 ")  Batches: %" PRId64 " (originally %" PRId64
								 ")  Memory Usage: %" PRId64 "kB",
								 b, ob, ba, oba, mem);
			return xasprintf("Buckets: %" PRId64 "  Batches: %" PRId64
							 "  Memory Usage: %" PRId64 "kB", b, ba, mem);
		}
		if (strcmp(label, "Storage Info") == 0)
			return xasprintf("Storage: %s  Maximum Storage: %" PRId64 "kB",
							 value_cstr(object_get(value, "Storage")),
							 value_i64(object_get(value, "Maximum Storage"), 0));
		if (strcmp(label, "Full-sort Groups") == 0 ||
			strcmp(label, "Pre-sorted Groups") == 0)
			return format_incremental_sort_group_text(label, value);
		if (strcmp(label, "Incremental Sort Groups") == 0)
		{
			Value	   *converted = value_object();
			Value	   *space;
			char	   *group_label;
			char	   *rendered;

			group_label = xasprintf("%s Groups",
									value_cstr(object_get(value, "Group Type")) ?
									value_cstr(object_get(value, "Group Type")) : "Sort");
			object_set(converted, "Group Count",
					   value_int(value_i64(object_get(value, "Group Count"), 0)));
			object_set(converted, "Sort Methods Used",
					   value_copy(object_get(value, "Sort Methods Used")));
			if (value_i64(object_get(value, "Peak Memory"), 0) > 0)
			{
				space = value_object();
				object_set(space, "Average Sort Space Used",
						   value_int(value_i64(object_get(value, "Average Memory"), 0)));
				object_set(space, "Peak Sort Space Used",
						   value_int(value_i64(object_get(value, "Peak Memory"), 0)));
				object_set(converted, "Sort Space Memory", space);
			}
			if (value_i64(object_get(value, "Peak Disk"), 0) > 0)
			{
				space = value_object();
				object_set(space, "Average Sort Space Used",
						   value_int(value_i64(object_get(value, "Average Disk"), 0)));
				object_set(space, "Peak Sort Space Used",
						   value_int(value_i64(object_get(value, "Peak Disk"), 0)));
				object_set(converted, "Sort Space Disk", space);
			}
			rendered = format_incremental_sort_group_text(group_label, converted);
			if (value_i64(object_get(value, "Worker"), -1) >= 0)
				return xasprintf("Worker %" PRId64 ":  %s",
								 value_i64(object_get(value, "Worker"), -1), rendered);
			return rendered;
		}
		if (strcmp(label, "Memoize Stats") == 0)
			return format_memoize_stats_text(value);
		if (strcmp(label, "Heap Blocks") == 0)
		{
			Buf			buf;

			buf_init(&buf);
			if (value_i64(object_get(value, "Exact Heap Blocks"), 0) > 0)
				buf_appendf(&buf, "exact=%" PRId64,
							value_i64(object_get(value, "Exact Heap Blocks"), 0));
			if (value_i64(object_get(value, "Lossy Heap Blocks"), 0) > 0)
			{
				if (buf.len > 0)
					buf_appendc(&buf, ' ');
				buf_appendf(&buf, "lossy=%" PRId64,
							value_i64(object_get(value, "Lossy Heap Blocks"), 0));
			}
			return buf_steal(&buf);
		}
		return json_compact_string(value);
	}
	if (value && value->type == V_BOOL)
		return xstrdup(value->v.b ? "true" : "false");
	if (value && value->type == V_STRING)
		return xstrdup(value->v.s);
	if (value && value->type == V_NUMERIC)
		return xstrdup(value->v.s);
	if (value && value->type == V_DOUBLE)
		return double_str(value->v.d);
	if (value && value->type == V_INT)
		return xasprintf("%" PRId64, value->v.i);
	if (value && value->type == V_UINT)
		return xasprintf("%" PRIu64, value->v.u);
	return xstrdup("null");
}

static char *
format_buffers_text(const Value *buffers)
{
	bool		has_shared;
	bool		has_local;
	bool		has_temp;
	Buf			buf;

	has_shared =
		value_i64(object_get(buffers, "Shared Hit Blocks"), 0) > 0 ||
		value_i64(object_get(buffers, "Shared Read Blocks"), 0) > 0 ||
		value_i64(object_get(buffers, "Shared Dirtied Blocks"), 0) > 0 ||
		value_i64(object_get(buffers, "Shared Written Blocks"), 0) > 0;
	has_local =
		value_i64(object_get(buffers, "Local Hit Blocks"), 0) > 0 ||
		value_i64(object_get(buffers, "Local Read Blocks"), 0) > 0 ||
		value_i64(object_get(buffers, "Local Dirtied Blocks"), 0) > 0 ||
		value_i64(object_get(buffers, "Local Written Blocks"), 0) > 0;
	has_temp =
		value_i64(object_get(buffers, "Temp Read Blocks"), 0) > 0 ||
		value_i64(object_get(buffers, "Temp Written Blocks"), 0) > 0;
	if (!(has_shared || has_local || has_temp))
		return NULL;
	buf_init(&buf);
	buf_appends(&buf, "Buffers:");
	if (has_shared)
	{
		buf_appends(&buf, " shared");
		if (value_i64(object_get(buffers, "Shared Hit Blocks"), 0) > 0)
			buf_appendf(&buf, " hit=%" PRId64,
						value_i64(object_get(buffers, "Shared Hit Blocks"), 0));
		if (value_i64(object_get(buffers, "Shared Read Blocks"), 0) > 0)
			buf_appendf(&buf, " read=%" PRId64,
						value_i64(object_get(buffers, "Shared Read Blocks"), 0));
		if (value_i64(object_get(buffers, "Shared Dirtied Blocks"), 0) > 0)
			buf_appendf(&buf, " dirtied=%" PRId64,
						value_i64(object_get(buffers, "Shared Dirtied Blocks"), 0));
		if (value_i64(object_get(buffers, "Shared Written Blocks"), 0) > 0)
			buf_appendf(&buf, " written=%" PRId64,
						value_i64(object_get(buffers, "Shared Written Blocks"), 0));
		if (has_local || has_temp)
			buf_appendc(&buf, ',');
	}
	if (has_local)
	{
		buf_appends(&buf, " local");
		if (value_i64(object_get(buffers, "Local Hit Blocks"), 0) > 0)
			buf_appendf(&buf, " hit=%" PRId64,
						value_i64(object_get(buffers, "Local Hit Blocks"), 0));
		if (value_i64(object_get(buffers, "Local Read Blocks"), 0) > 0)
			buf_appendf(&buf, " read=%" PRId64,
						value_i64(object_get(buffers, "Local Read Blocks"), 0));
		if (value_i64(object_get(buffers, "Local Dirtied Blocks"), 0) > 0)
			buf_appendf(&buf, " dirtied=%" PRId64,
						value_i64(object_get(buffers, "Local Dirtied Blocks"), 0));
		if (value_i64(object_get(buffers, "Local Written Blocks"), 0) > 0)
			buf_appendf(&buf, " written=%" PRId64,
						value_i64(object_get(buffers, "Local Written Blocks"), 0));
		if (has_temp)
			buf_appendc(&buf, ',');
	}
	if (has_temp)
	{
		buf_appends(&buf, " temp");
		if (value_i64(object_get(buffers, "Temp Read Blocks"), 0) > 0)
			buf_appendf(&buf, " read=%" PRId64,
						value_i64(object_get(buffers, "Temp Read Blocks"), 0));
		if (value_i64(object_get(buffers, "Temp Written Blocks"), 0) > 0)
			buf_appendf(&buf, " written=%" PRId64,
						value_i64(object_get(buffers, "Temp Written Blocks"), 0));
	}
	return buf_steal(&buf);
}

static char *
format_io_timings_text(const Value *buffers)
{
	static const char *groups[] = {"shared", "local", "temp"};
	static const char *keys[][2] = {
		{"Shared Read Time", "Shared Write Time"},
		{"Local Read Time", "Local Write Time"},
		{"Temp Read Time", "Temp Write Time"},
	};
	Buf			buf;
	bool		any = false;

	buf_init(&buf);
	for (int i = 0; i < 3; i++)
	{
		double		read_time = value_double_as(object_get(buffers, keys[i][0]), 0);
		double		write_time = value_double_as(object_get(buffers, keys[i][1]), 0);

		if (read_time <= 0 && write_time <= 0)
			continue;
		if (!any)
			buf_appends(&buf, "I/O Timings: ");
		else
			buf_appends(&buf, ", ");
		any = true;
		buf_appends(&buf, groups[i]);
		if (read_time > 0)
			buf_appendf(&buf, " read=%.3f", read_time);
		if (write_time > 0)
			buf_appendf(&buf, " write=%.3f", write_time);
	}
	return any ? buf_steal(&buf) : NULL;
}

static char *
format_wal_text(const Value *wal)
{
	Buf			buf;

	buf_init(&buf);
	buf_appends(&buf, "WAL:");
	if (value_i64(object_get(wal, "WAL Records"), 0) > 0)
		buf_appendf(&buf, " records=%" PRId64, value_i64(object_get(wal, "WAL Records"), 0));
	if (value_i64(object_get(wal, "WAL FPI"), 0) > 0)
		buf_appendf(&buf, " fpi=%" PRId64, value_i64(object_get(wal, "WAL FPI"), 0));
	if (value_i64(object_get(wal, "WAL Bytes"), 0) > 0)
		buf_appendf(&buf, " bytes=%" PRId64, value_i64(object_get(wal, "WAL Bytes"), 0));
	if (value_i64(object_get(wal, "WAL Buffers Full"), 0) > 0)
		buf_appendf(&buf, " buffers full=%" PRId64,
					value_i64(object_get(wal, "WAL Buffers Full"), 0));
	return strcmp(buf.data ? buf.data : "", "WAL:") == 0 ? NULL : buf_steal(&buf);
}

static void
append_line(Buf *buf, const char *line)
{
	if (buf->len > 0)
		buf_appendc(buf, '\n');
	buf_appends(buf, line);
}

static char *
format_sampling_text(const Value *node)
{
	Value	   *params = object_get(node, "Sampling Parameters");
	char	   *joined;
	Buf			buf;

	if (params == NULL || params->type != V_ARRAY)
	{
		Value	   *tmp = value_array();

		if (params)
			array_append(tmp, value_copy(params));
		params = tmp;
	}
	joined = join_string_array(params, ", ");
	buf_init(&buf);
	buf_appendf(&buf, "Sampling: %s (%s)",
				value_cstr(object_get(node, "Sampling Method")), joined);
	if (value_truthy(object_get(node, "Repeatable Seed")))
		buf_appendf(&buf, " REPEATABLE (%s)",
					value_cstr(object_get(node, "Repeatable Seed")));
	return buf_steal(&buf);
}

static char *
plan_line(const Value *node, bool timing)
{
	uint32_t	flags = (uint32_t) value_u64(object_get(node, "Flags"), 0);
	char	   *json_name;
	char	   *name;
	Value	   *props;
	Buf			buf;
	const char *ntype = value_cstr(object_get(node, "Node Type"));

	postgres_node_names(node, &json_name, &name, &props);
	if (ntype && strcmp(ntype, "Custom Scan") == 0 &&
		value_truthy(object_get(node, "Custom Plan Provider")))
	{
		char	   *old = name;

		name = xasprintf("%s (%s)", old,
						 value_cstr(object_get(node, "Custom Plan Provider")));
	}
	if (flags & NODE_PARALLEL_AWARE)
	{
		char	   *old = name;

		name = xasprintf("Parallel %s", old);
	}
	if (flags & NODE_ASYNC_CAPABLE)
	{
		char	   *old = name;

		name = xasprintf("Async %s", old);
	}
	if (ntype && (strcmp(ntype, "Index Scan") == 0 ||
				  strcmp(ntype, "Index Only Scan") == 0) &&
		strcmp(scan_direction(node), "Backward") == 0)
	{
		char	   *old = name;

		name = xasprintf("%s Backward", old);
	}
	if (value_truthy(object_get(node, "Index Name")))
	{
		char	   *q = quote_identifier(value_cstr(object_get(node, "Index Name")), false);
		char	   *old = name;

		if (ntype && strcmp(ntype, "Bitmap Index Scan") == 0)
			name = xasprintf("%s on %s", old, q);
		else
			name = xasprintf("%s using %s", old, q);
	}
	if (value_truthy(object_get(node, "Relation Name")))
	{
		char	   *rel = quote_identifier(value_cstr(object_get(node, "Relation Name")), false);
		char	   *old = name;

		if (value_truthy(object_get(node, "Schema")))
		{
			char	   *schema = quote_identifier(value_cstr(object_get(node, "Schema")), false);
			char	   *tmp = xasprintf("%s.%s", schema, rel);

			rel = tmp;
		}
		name = xasprintf("%s on %s", old, rel);
		if (value_truthy(object_get(node, "Alias")) &&
			strcmp(value_cstr(object_get(node, "Alias")),
				   value_cstr(object_get(node, "Relation Name"))) != 0)
		{
			char	   *alias = quote_identifier(value_cstr(object_get(node, "Alias")), false);

			old = name;
			name = xasprintf("%s %s", old, alias);
		}
	}
	else if (ntype && strcmp(ntype, "Function Scan") == 0)
	{
		const char *alias = value_cstr(object_get(node, "Alias"));
		const char *objectname = value_cstr(object_get(node, "Function Name"));

		if (objectname)
		{
			char	   *target = quote_identifier(objectname, false);
			char	   *old = name;

			if (value_truthy(object_get(node, "Schema")))
			{
				char	   *schema = quote_identifier(value_cstr(object_get(node, "Schema")), false);

				target = xasprintf("%s.%s", schema, target);
			}
			name = xasprintf("%s on %s", old, target);
			if (alias && strcmp(alias, objectname) != 0)
			{
				char	   *qalias = quote_identifier(alias, false);

				old = name;
				name = xasprintf("%s %s", old, qalias);
			}
		}
		else if (alias)
		{
			char	   *qalias = quote_identifier(alias, false);
			char	   *old = name;

			name = xasprintf("%s on %s", old, qalias);
		}
	}
	else if (ntype && strcmp(ntype, "Table Function Scan") == 0)
	{
		const char *alias = value_cstr(object_get(node, "Alias"));
		const char *objectname = value_cstr(object_get(node, "Table Function Name"));

		if (objectname)
		{
			char	   *target = quote_identifier(objectname, true);
			char	   *old = name;

			name = xasprintf("%s on %s", old, target);
			if (alias && strcmp(alias, objectname) != 0)
			{
				char	   *qalias = quote_identifier(alias, false);

				old = name;
				name = xasprintf("%s %s", old, qalias);
			}
		}
		else if (alias)
		{
			char	   *qalias = quote_identifier(alias, false);
			char	   *old = name;

			name = xasprintf("%s on %s", old, qalias);
		}
	}
	else if (ntype && (strcmp(ntype, "CTE Scan") == 0 ||
					   strcmp(ntype, "WorkTable Scan") == 0 ||
					   strcmp(ntype, "Named Tuplestore Scan") == 0))
	{
		const char *alias = value_cstr(object_get(node, "Alias"));
		const char *objectname = value_cstr(object_get(node,
													   strcmp(ntype, "Named Tuplestore Scan") == 0 ?
													   "Tuplestore Name" : "CTE Name"));

		if (objectname)
		{
			char	   *target = quote_identifier(objectname, false);
			char	   *old = name;

			name = xasprintf("%s on %s", old, target);
			if (alias && strcmp(alias, objectname) != 0)
			{
				char	   *qalias = quote_identifier(alias, false);

				old = name;
				name = xasprintf("%s %s", old, qalias);
			}
		}
		else if (alias)
		{
			char	   *qalias = quote_identifier(alias, false);
			char	   *old = name;

			name = xasprintf("%s on %s", old, qalias);
		}
	}
	else if (ntype && (strcmp(ntype, "Subquery Scan") == 0 ||
					   strcmp(ntype, "Values Scan") == 0) &&
			 value_truthy(object_get(node, "Alias")))
	{
		char	   *qalias = quote_identifier(value_cstr(object_get(node, "Alias")), false);
		char	   *old = name;

		name = xasprintf("%s on %s", old, qalias);
	}
	buf_init(&buf);
	buf_appendf(&buf, "%s  (cost=%.2f..%.2f rows=%.0f width=%" PRId64 ")",
				name,
				value_double_as(object_get(node, "Startup Cost"), 0),
				value_double_as(object_get(node, "Total Cost"), 0),
				value_double_as(object_get(node, "Plan Rows"), 0),
				value_i64(object_get(node, "Plan Width"), 0));
	if (object_has(node, "Actual Rows"))
	{
		if (timing)
			buf_appendf(&buf, " (actual time=%.3f..%.3f rows=%.2f loops=%.0f)",
						value_double_as(object_get(node, "Startup Time"), 0),
						value_double_as(object_get(node, "Total Time"), 0),
						value_double_as(object_get(node, "Actual Rows"), 0),
						value_double_as(object_get(node, "Actual Loops"), 0));
		else
			buf_appendf(&buf, " (actual rows=%.2f loops=%.0f)",
						value_double_as(object_get(node, "Actual Rows"), 0),
						value_double_as(object_get(node, "Actual Loops"), 0));
	}
	else if (value_truthy(object_get(node, "Never Executed")))
		buf_appends(&buf, " (never executed)");
	return buf_steal(&buf);
}

static bool
label_in_text_detail_order(const char *label)
{
	static const char *order[] = {
		"Output", "Function Call", "Table Function Call", "Window",
		"Inner Unique", "Index Cond", "Recheck Cond", "TID Cond", "Order By",
		"Hash Cond", "Merge Cond", "Join Filter", "Sampling Method", "Filter",
		"One-Time Filter", "Run Condition", "Rows Removed by Filter",
		"Rows Removed by Join Filter", "Rows Removed by Index Recheck",
		"Rows Removed by Conflict Filter", "Sort Key", "Presorted Key",
		"Group Key", "Hash Key", "Heap Fetches", "Workers Planned",
		"Workers Launched", "Single Copy", "Index Searches", "Subplans Removed",
		"Cache Key", "Cache Mode", "Conflict Resolution",
		"Conflict Arbiter Index", "Conflict Filter", "Merge Actions",
	};

	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
		if (strcmp(label, order[i]) == 0)
			return true;
	return false;
}

static void
render_plan_text(Buf *buf, const Value *node, uint32_t record_flags, int indent)
{
	static const char *detail_order[] = {
		"Output", "Function Call", "Table Function Call", "Window",
		"Inner Unique", "Index Cond", "Recheck Cond", "TID Cond", "Order By",
		"Hash Cond", "Merge Cond", "Join Filter", "Sampling Method", "Filter",
		"One-Time Filter", "Run Condition", "Rows Removed by Filter",
		"Rows Removed by Join Filter", "Rows Removed by Index Recheck",
		"Rows Removed by Conflict Filter", "Sort Key", "Presorted Key",
		"Group Key", "Hash Key", "Heap Fetches", "Workers Planned",
		"Workers Launched", "Single Copy", "Index Searches", "Subplans Removed",
		"Cache Key", "Cache Mode", "Conflict Resolution",
		"Conflict Arbiter Index", "Conflict Filter", "Merge Actions",
	};
	static const char *second_order[] = {
		"Sort Info", "Hash Info", "Storage Info", "Heap Blocks",
		"Memoize Stats", "HashAgg Stats", "Incremental Sort Groups",
		"Full-sort Groups", "Pre-sorted Groups", "Conflict Tuples",
		"Merge Tuples",
	};
	int			detail_indent = indent + (indent == 0 ? 2 : 6);
	int			child_indent = detail_indent;
	bool		timing = (record_flags & FLAG_TIMING) != 0;
	char	   *line = plan_line(node, timing);
	Buf			linebuf;

	buf_init(&linebuf);
	for (int i = 0; i < indent; i++)
		buf_appendc(&linebuf, ' ');
	if (indent)
		buf_appends(&linebuf, "->  ");
	buf_appends(&linebuf, line);
	append_line(buf, linebuf.data ? linebuf.data : "");

	for (size_t i = 0; i < sizeof(detail_order) / sizeof(detail_order[0]); i++)
	{
		const char *label = detail_order[i];
		Value	   *values = object_get(node, label);
		Value	   *tmp = NULL;
		const Value *array = values;

		if (values == NULL)
			continue;
		if (strcmp(label, "Inner Unique") == 0 &&
			!((record_flags & FLAG_VERBOSE) && value_truthy(values)))
			continue;
		if (strcmp(label, "Single Copy") == 0 && !value_truthy(values))
			continue;
		if (strcmp(label, "Sampling Method") == 0)
		{
			char	   *s = format_sampling_text(node);
			Buf			l;

			buf_init(&l);
			for (int j = 0; j < detail_indent; j++)
				buf_appendc(&l, ' ');
			buf_appends(&l, s);
			append_line(buf, l.data);
			continue;
		}
		if (values->type != V_ARRAY || value_is_string_array(values))
		{
			tmp = value_array();
			array_append(tmp, value_copy(values));
			array = tmp;
		}
		for (size_t j = 0; j < array_len(array); j++)
		{
			Value	   *value = array_get(array, j);
			char	   *rendered;
			Buf			l;

			if (strncmp(label, "Rows Removed by", strlen("Rows Removed by")) == 0 &&
				value_double_as(value, 0) <= 0)
				continue;
			if (strcmp(label, "Subplans Removed") == 0 &&
				value_double_as(value, 0) <= 0)
				continue;
			rendered = render_value_text(label, value);
			buf_init(&l);
			for (int k = 0; k < detail_indent; k++)
				buf_appendc(&l, ' ');
			if (strcmp(label, "Sort Info") == 0 ||
				strcmp(label, "Hash Info") == 0 ||
				strcmp(label, "Storage Info") == 0)
				buf_appends(&l, rendered);
			else
				buf_appendf(&l, "%s: %s", label, rendered);
			append_line(buf, l.data);
		}
	}
	for (size_t i = 0; i < sizeof(second_order) / sizeof(second_order[0]); i++)
	{
		const char *label = second_order[i];
		Value	   *values = object_get(node, label);
		Value	   *tmp = NULL;
		const Value *array = values;

		if (values == NULL)
			continue;
		if (values->type != V_ARRAY)
		{
			tmp = value_array();
			array_append(tmp, value_copy(values));
			array = tmp;
		}
		for (size_t j = 0; j < array_len(array); j++)
		{
			Value	   *value = array_get(array, j);
			char	   *rendered = render_value_text(label, value);
			Buf			l;

			if (strcmp(label, "Heap Blocks") == 0 && rendered[0] == '\0')
				continue;
			if (strcmp(label, "Sort Info") == 0 && value &&
				value->type == V_OBJECT &&
				value_i64(object_get(value, "Worker"), -1) >= 0)
				continue;
			buf_init(&l);
			for (int k = 0; k < detail_indent; k++)
				buf_appendc(&l, ' ');
			if (strcmp(label, "Sort Info") == 0 ||
				strcmp(label, "Hash Info") == 0 ||
				strcmp(label, "Storage Info") == 0 ||
				strcmp(label, "Memoize Stats") == 0 ||
				strcmp(label, "Incremental Sort Groups") == 0 ||
				strcmp(label, "Full-sort Groups") == 0 ||
				strcmp(label, "Pre-sorted Groups") == 0)
				buf_appends(&l, rendered);
			else
				buf_appendf(&l, "%s: %s", label, rendered);
			append_line(buf, l.data);
		}
	}
	if (object_has(node, "Extension Explain"))
	{
		Value	   *values = object_get(node, "Extension Explain");
		Value	   *tmp = NULL;
		const Value *array = values;

		if (values->type != V_ARRAY)
		{
			tmp = value_array();
			array_append(tmp, value_copy(values));
			array = tmp;
		}
		for (size_t i = 0; i < array_len(array); i++)
		{
			const char *text = value_cstr(array_get(array, i));
			const char *p = text ? text : "";

			while (*p)
			{
				const char *end = strchr(p, '\n');
				Buf			l;

				if (end == NULL)
					end = p + strlen(p);
				buf_init(&l);
				for (int k = 0; k < detail_indent; k++)
					buf_appendc(&l, ' ');
				buf_append(&l, p, (size_t) (end - p));
				append_line(buf, l.data);
				p = *end ? end + 1 : end;
			}
		}
	}
	if (value_u64(object_get(node, "Flags"), 0) & NODE_DISABLED)
	{
		Buf			l;

		buf_init(&l);
		for (int k = 0; k < detail_indent; k++)
			buf_appendc(&l, ' ');
		buf_appends(&l, "Disabled: true");
		append_line(buf, l.data);
	}
	if (object_has(node, "Buffers"))
	{
		char	   *s = format_buffers_text(object_get(node, "Buffers"));

		if (s)
		{
			Buf			l;

			buf_init(&l);
			for (int k = 0; k < detail_indent; k++)
				buf_appendc(&l, ' ');
			buf_appends(&l, s);
			append_line(buf, l.data);
		}
		s = format_io_timings_text(object_get(node, "Buffers"));
		if (s)
		{
			Buf			l;

			buf_init(&l);
			for (int k = 0; k < detail_indent; k++)
				buf_appendc(&l, ' ');
			buf_appends(&l, s);
			append_line(buf, l.data);
		}
	}
	if (object_has(node, "WAL"))
	{
		char	   *s = format_wal_text(object_get(node, "WAL"));

		if (s)
		{
			Buf			l;

			buf_init(&l);
			for (int k = 0; k < detail_indent; k++)
				buf_appendc(&l, ' ');
			buf_appends(&l, s);
			append_line(buf, l.data);
		}
	}
	if (object_has(node, "Sort Info"))
	{
		Value	   *values = object_get(node, "Sort Info");
		Value	   *tmp = NULL;
		const Value *array = values;

		if (values->type != V_ARRAY)
		{
			tmp = value_array();
			array_append(tmp, value_copy(values));
			array = tmp;
		}
		for (size_t i = 0; i < array_len(array); i++)
		{
			Value	   *value = array_get(array, i);

			if (value && value->type == V_OBJECT &&
				value_i64(object_get(value, "Worker"), -1) >= 0)
			{
				char	   *rendered = render_value_text("Sort Info", value);
				Buf			l;

				buf_init(&l);
				for (int k = 0; k < detail_indent; k++)
					buf_appendc(&l, ' ');
				buf_appends(&l, rendered);
				append_line(buf, l.data);
			}
		}
	}
	if (object_get(node, "Plans") && object_get(node, "Plans")->type == V_ARRAY)
	{
		Value	   *plans = object_get(node, "Plans");

		for (size_t i = 0; i < plans->v.a->len; i++)
		{
			Value	   *child = plans->v.a->items[i];

			if (value_truthy(object_get(child, "Subplan Name")))
			{
				Buf			l;

				buf_init(&l);
				for (int k = 0; k < child_indent; k++)
					buf_appendc(&l, ' ');
				buf_appends(&l, value_cstr(object_get(child, "Subplan Name")));
				append_line(buf, l.data);
				render_plan_text(buf, child, record_flags, child_indent + 2);
			}
			else
				render_plan_text(buf, child, record_flags, child_indent);
		}
	}
	(void) label_in_text_detail_order;
}

static void
render_planning_text(Buf *buf, const Value *planning)
{
	char	   *s = format_buffers_text(planning);

	if (s == NULL)
		s = format_io_timings_text(planning);
	if (s == NULL)
		return;
	append_line(buf, "Planning:");
	s = format_buffers_text(planning);
	if (s)
	{
		char	   *line = xasprintf("  %s", s);

		append_line(buf, line);
	}
	s = format_io_timings_text(planning);
	if (s)
	{
		char	   *line = xasprintf("  %s", s);

		append_line(buf, line);
	}
}

static void
render_trigger_text(Buf *buf, const Value *trigger, uint32_t flags)
{
	const char *name = value_cstr(object_get(trigger, "Trigger Name"));
	const char *rel = value_cstr(object_get(trigger, "Relation"));
	const char *con = value_cstr(object_get(trigger, "Constraint Name"));
	Buf			line;

	if (name == NULL || name[0] == '\0')
		name = "Trigger";
	buf_init(&line);
	buf_appendf(&line, "Trigger %s", name);
	if (con && con[0])
		buf_appendf(&line, " for constraint %s", con);
	if (rel && rel[0])
		buf_appendf(&line, " on %s", rel);
	if (flags & FLAG_TIMING)
		buf_appendf(&line, ": time=%.3f calls=%.0f",
					value_double_as(object_get(trigger, "Time"), 0),
					value_double_as(object_get(trigger, "Calls"), 0));
	else
		buf_appendf(&line, ": calls=%.0f",
					value_double_as(object_get(trigger, "Calls"), 0));
	append_line(buf, line.data);
}

static void
render_jit_text(Buf *buf, const Value *jit)
{
	Value	   *opts = object_get(jit, "Options");
	Value	   *timing = object_get(jit, "Timing");

	append_line(buf, "JIT:");
	append_line(buf, xasprintf("  Functions: %" PRId64,
							   value_i64(object_get(jit, "Functions"), 0)));
	append_line(buf, xasprintf("  Options: Inlining %s, Optimization %s, Expressions %s, Deforming %s",
							   value_truthy(object_get(opts, "Inlining")) ? "true" : "false",
							   value_truthy(object_get(opts, "Optimization")) ? "true" : "false",
							   value_truthy(object_get(opts, "Expressions")) ? "true" : "false",
							   value_truthy(object_get(opts, "Deforming")) ? "true" : "false"));
	if (timing && value_truthy(object_get(timing, "Available")))
		append_line(buf, xasprintf("  Timing: Generation %.3f ms (Deform %.3f ms), Inlining %.3f ms, Optimization %.3f ms, Emission %.3f ms, Total %.3f ms",
								   value_double_as(object_get(timing, "Generation"), 0),
								   value_double_as(object_get(timing, "Deform"), 0),
								   value_double_as(object_get(timing, "Inlining"), 0),
								   value_double_as(object_get(timing, "Optimization"), 0),
								   value_double_as(object_get(timing, "Emission"), 0),
								   value_double_as(object_get(timing, "Total"), 0)));
}

static void
append_padded(Buf *buf, const char *value, int padding)
{
	size_t		len;
	int			spaces;

	if (value == NULL)
		value = "";
	len = strlen(value);
	if (padding > 0 && padding > (int) len)
	{
		spaces = padding - (int) len;
		while (spaces-- > 0)
			buf_appendc(buf, ' ');
	}
	buf_appends(buf, value);
	if (padding < 0 && -padding > (int) len)
	{
		spaces = -padding - (int) len;
		while (spaces-- > 0)
			buf_appendc(buf, ' ');
	}
}

static const char *
parse_log_prefix_padding(const char *p, int *padding)
{
	int			sign = 1;
	int			value = 0;
	bool		have_digit = false;

	if (*p == '-')
	{
		sign = -1;
		p++;
	}
	while (isdigit((unsigned char) *p))
	{
		have_digit = true;
		value = value * 10 + (*p - '0');
		p++;
	}
	if (!have_digit)
		return NULL;
	*padding = sign * value;
	return p;
}

static void
append_unavailable_log_prefix_token(char token)
{
	fatal("log_line_prefix contains %%%c, but auto_explain_z records do not store that PostgreSQL log field exactly",
		  token);
}

static void
render_log_line_prefix(Buf *buf, const char *format, const Value *record)
{
	const Value *ctx = object_get(record, "Log Context");
	const char *timestamp = value_cstr(object_get(ctx, "Timestamp"));
	const char *backend_start = value_cstr(object_get(ctx, "Backend Start"));
	uint64_t	pid = value_u64(object_get(ctx, "PID"), 0);
	const char *p;

	if (format == NULL)
		return;
	for (p = format; *p != '\0'; p++)
	{
		int			padding;
		char		token;
		char	   *tmp = NULL;
		const char *value = NULL;

		if (*p != '%')
		{
			buf_appendc(buf, *p);
			continue;
		}
		p++;
		if (*p == '\0')
			break;
		if (*p == '%')
		{
			buf_appendc(buf, '%');
			continue;
		}
		if (*p > '9')
			padding = 0;
		else
		{
			p = parse_log_prefix_padding(p, &padding);
			if (p == NULL || *p == '\0')
				break;
		}
		token = *p;
		switch (token)
		{
			case 'a':
				value = value_cstr(object_get(ctx, "Application Name"));
				if (value == NULL || value[0] == '\0')
					value = "[unknown]";
				break;
			case 'b':
				value = value_cstr(object_get(ctx, "Backend Type"));
				break;
			case 'u':
				value = value_cstr(object_get(ctx, "User"));
				if (value == NULL || value[0] == '\0')
					value = "[unknown]";
				break;
			case 'd':
				value = value_cstr(object_get(ctx, "Database"));
				if (value == NULL || value[0] == '\0')
					value = "[unknown]";
				break;
			case 'c':
				tmp = format_session_id(ctx);
				value = tmp;
				break;
			case 'p':
				tmp = xasprintf("%" PRIu64, pid);
				value = tmp;
				break;
			case 'm':
				tmp = format_log_timestamp_ms(timestamp);
				value = tmp;
				break;
			case 't':
				tmp = format_log_timestamp(timestamp);
				value = tmp;
				break;
			case 'n':
				tmp = format_log_epoch_ms(timestamp);
				value = tmp;
				break;
			case 's':
				tmp = format_log_timestamp(backend_start);
				value = tmp;
				break;
			case 'r':
				{
					const char *host = value_cstr(object_get(ctx, "Client Host"));
					const char *port = value_cstr(object_get(ctx, "Client Port"));

					if (host && host[0] != '\0' && port && port[0] != '\0')
						tmp = xasprintf("%s(%s)", host, port);
					else if (host && host[0] != '\0')
						tmp = xstrdup(host);
					else
						tmp = xstrdup("");
					value = tmp;
					break;
				}
			case 'h':
				value = value_cstr(object_get(ctx, "Client Host"));
				break;
			case 'q':
				value = "";
				break;
			case 'e':
				value = "00000";
				break;
			case 'Q':
				tmp = xasprintf("%" PRId64,
								pg_signed_i64(value_u64(object_get(record, "Query Identifier"), 0)));
				value = tmp;
				break;
			case 'i':
			case 'L':
			case 'P':
			case 'l':
			case 'v':
			case 'x':
				append_unavailable_log_prefix_token(token);
				break;
			default:
				value = "";
				break;
		}
		append_padded(buf, value, padding);
	}
}

static void
append_with_tabs(Buf *buf, const char *str)
{
	char		ch;

	while ((ch = *str++) != '\0')
	{
		buf_appendc(buf, ch);
		if (ch == '\n')
			buf_appendc(buf, '\t');
	}
}

static void
render_record_payload(Buf *buf, const Value *record, OutputFormat format)
{
	Value	   *single;
	Value	   *converted;

	switch (format)
	{
		case FMT_TEXT:
			single = value_array();
			array_append(single, (Value *) record);
			render_postgres_text(buf, single);
			break;
		case FMT_JSON:
			converted = postgres_record(record);
			render_json_value(buf, converted, 0, NULL);
			break;
		case FMT_YAML:
			converted = postgres_record(record);
			render_yaml_obj(buf, converted, 0);
			if (buf->len > 0 && buf->data[buf->len - 1] == '\n')
				buf->data[--buf->len] = '\0';
			break;
		case FMT_XML:
			converted = postgres_record(record);
			single = value_array();
			array_append(single, converted);
			render_xml_document(buf, single);
			break;
	}
}

static void
render_postgres_log(Buf *buf, const Value *records, OutputFormat format,
					const ServerLogConfig *config)
{
	for (size_t i = 0; i < array_len(records); i++)
	{
		Value	   *rec = array_get(records, i);
		Buf			payload;
		char	   *message;

		if (i > 0)
			buf_appendc(buf, '\n');
		render_log_line_prefix(buf, config->log_line_prefix, rec);
		buf_appends(buf, "LOG:  ");
		if (config->verbose)
			buf_appends(buf, "00000: ");

		buf_init(&payload);
		render_record_payload(&payload, rec, format);
		message = xasprintf("duration: %.3f ms  plan:\n%s",
							value_double_as(object_get(rec, "Duration"), 0),
							payload.data ? payload.data : "");
		append_with_tabs(buf, message);
	}
}

static void
render_postgres_text(Buf *buf, const Value *records)
{
	for (size_t i = 0; i < array_len(records); i++)
	{
		Value	   *rec = array_get(records, i);
		uint32_t	flags = (uint32_t) value_u64(object_get(rec, "Flags"), 0);

		if (i > 0)
			buf_appends(buf, "\n\n");
		if (value_truthy(object_get(rec, "Query Text")))
			append_line(buf, xasprintf("Query Text: %s",
									   value_cstr(object_get(rec, "Query Text"))));
		if (value_truthy(object_get(rec, "Query Parameters")))
			append_line(buf, xasprintf("Query Parameters: %s",
									   value_cstr(object_get(rec, "Query Parameters"))));
		render_plan_text(buf, object_get(rec, "Plan"), flags, 0);
		if ((flags & FLAG_VERBOSE) && value_truthy(object_get(rec, "Query Identifier")))
			append_line(buf, xasprintf("Query Identifier: %" PRId64,
									   pg_signed_i64(value_u64(object_get(rec, "Query Identifier"), 0))));
		if (object_has(rec, "Planning"))
			render_planning_text(buf, object_get(rec, "Planning"));
		if (object_has(rec, "Trigger"))
		{
			Value	   *trig = object_get(rec, "Trigger");

			if (trig->type == V_ARRAY)
			{
				for (size_t j = 0; j < trig->v.a->len; j++)
					render_trigger_text(buf, trig->v.a->items[j], flags);
			}
			else
				render_trigger_text(buf, trig, flags);
		}
		if (object_has(rec, "JIT"))
			render_jit_text(buf, object_get(rec, "JIT"));
	}
}

static void
render_raw_text(Buf *buf, const Value *records)
{
	for (size_t i = 0; i < array_len(records); i++)
	{
		Value	   *rec = array_get(records, i);
		Value	   *ctx = object_get(rec, "Log Context");
		const char *ts = value_cstr(object_get(ctx, "Timestamp"));

		if (i > 0)
			buf_appends(buf, "\n\n");
		append_line(buf, xasprintf("%s pid=%" PRId64 " user=%s db=%s app=%s queryid=%" PRIu64 " duration=%.3f ms",
								   ts ? ts : value_cstr(object_get(object_get(rec, "Record"), "Timestamp")),
								   value_i64(object_get(ctx, "PID"), 0),
								   value_cstr(object_get(ctx, "User")),
								   value_cstr(object_get(ctx, "Database")),
								   value_cstr(object_get(ctx, "Application Name")),
								   value_u64(object_get(rec, "Query Identifier"), 0),
								   value_double_as(object_get(rec, "Duration"), 0)));
		if (value_truthy(object_get(rec, "Query Text")))
			append_line(buf, xasprintf("Query Text: %s",
									   value_cstr(object_get(rec, "Query Text"))));
		if (value_truthy(object_get(rec, "Query Parameters")))
			append_line(buf, xasprintf("Query Parameters: %s",
									   value_cstr(object_get(rec, "Query Parameters"))));
		render_plan_text(buf, object_get(rec, "Plan"),
						 (uint32_t) value_u64(object_get(rec, "Flags"), 0), 0);
		if (object_has(rec, "Planning"))
			render_planning_text(buf, object_get(rec, "Planning"));
		if (object_has(rec, "Trigger"))
		{
			Value	   *trig = object_get(rec, "Trigger");

			if (trig->type == V_ARRAY)
			{
				for (size_t j = 0; j < trig->v.a->len; j++)
					render_trigger_text(buf, trig->v.a->items[j], FLAG_TIMING);
			}
			else
				render_trigger_text(buf, trig, FLAG_TIMING);
		}
		if (object_has(rec, "JIT"))
			render_jit_text(buf, object_get(rec, "JIT"));
	}
}

static void
render_xml_document(Buf *buf, const Value *records)
{
	buf_appends(buf, "<explain xmlns=\"http://www.postgresql.org/2009/explain\">\n");
	for (size_t i = 0; i < array_len(records); i++)
	{
		Value	   *record = array_get(records, i);

		buf_appends(buf, "  <Query>\n");
		for (size_t j = 0; j < record->v.o->len; j++)
			render_xml_value(buf, record->v.o->pairs[j].key,
							 record->v.o->pairs[j].value, 2);
		buf_appends(buf, "  </Query>\n");
	}
	buf_appends(buf, "</explain>");
}

static void
usage(FILE *out)
{
	fprintf(out,
			"usage: auto_explain_z_dump [-f FORMAT|--format FORMAT] [--raw]\n"
			"                           [--postgres-log]\n"
			"                           [--log-line-prefix PREFIX]\n"
			"                           [--log-timezone TZ]\n"
			"                           [--log-error-verbosity VALUE]\n"
			"                           [--verify-crc] FILE...\n"
			"\n"
			"Decode auto_explain_z binary logs.\n"
			"\n"
			"  -f, --format FORMAT   text, json, yaml, or xml (default: text)\n"
			"  --raw                emit AEZ diagnostic records\n"
			"  --postgres-log       emit PostgreSQL auto_explain-style LOG records,\n"
			"                       using offline PostgreSQL log formatting options\n"
			"  --log-line-prefix P  PostgreSQL log_line_prefix (default: \"%s\")\n"
			"  --log-timezone TZ    timezone for %%m/%%t/%%s (default: %s)\n"
			"  --log-error-verbosity VALUE\n"
			"                       default, terse, or verbose (default: default)\n"
			"  --verify-crc         verify legacy v3 CRC32C records\n",
			DEFAULT_LOG_LINE_PREFIX,
			DEFAULT_LOG_TIMEZONE);
}

static OutputFormat
parse_format(const char *format)
{
	if (strcmp(format, "text") == 0)
		return FMT_TEXT;
	if (strcmp(format, "json") == 0)
		return FMT_JSON;
	if (strcmp(format, "yaml") == 0)
		return FMT_YAML;
	if (strcmp(format, "xml") == 0)
		return FMT_XML;
	fatal("unknown format: %s", format);
}

static Options
parse_options(int argc, char **argv)
{
	Options		opts;
	int			i;

	opts.format = FMT_TEXT;
	opts.raw = false;
	opts.verify_crc = false;
	opts.postgres_log = false;
	opts.log_line_prefix = DEFAULT_LOG_LINE_PREFIX;
	opts.log_timezone = DEFAULT_LOG_TIMEZONE;
	opts.log_error_verbosity = DEFAULT_LOG_ERROR_VERBOSITY;
	opts.first_file_arg = argc;
	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
		{
			usage(stdout);
			exit(0);
		}
		else if (strcmp(argv[i], "--raw") == 0)
			opts.raw = true;
		else if (strcmp(argv[i], "--postgres-log") == 0)
			opts.postgres_log = true;
		else if (strcmp(argv[i], "--log-line-prefix") == 0)
		{
			if (i + 1 >= argc)
				fatal("%s requires an argument", argv[i]);
			opts.log_line_prefix = argv[++i];
		}
		else if (strncmp(argv[i], "--log-line-prefix=", 18) == 0)
			opts.log_line_prefix = argv[i] + 18;
		else if (strcmp(argv[i], "--log-timezone") == 0)
		{
			if (i + 1 >= argc)
				fatal("%s requires an argument", argv[i]);
			opts.log_timezone = argv[++i];
		}
		else if (strncmp(argv[i], "--log-timezone=", 15) == 0)
			opts.log_timezone = argv[i] + 15;
		else if (strcmp(argv[i], "--log-error-verbosity") == 0)
		{
			if (i + 1 >= argc)
				fatal("%s requires an argument", argv[i]);
			opts.log_error_verbosity = argv[++i];
		}
		else if (strncmp(argv[i], "--log-error-verbosity=", 22) == 0)
			opts.log_error_verbosity = argv[i] + 22;
		else if (strcmp(argv[i], "--verify-crc") == 0)
			opts.verify_crc = true;
		else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--format") == 0)
		{
			if (i + 1 >= argc)
				fatal("%s requires an argument", argv[i]);
			opts.format = parse_format(argv[++i]);
		}
		else if (strncmp(argv[i], "--format=", 9) == 0)
			opts.format = parse_format(argv[i] + 9);
		else if (argv[i][0] == '-')
			fatal("unknown option: %s", argv[i]);
		else
		{
			opts.first_file_arg = i;
			break;
		}
	}
	if (opts.raw && opts.postgres_log)
		fatal("--raw cannot be combined with --postgres-log");
	if (opts.first_file_arg >= argc)
	{
		usage(stderr);
		exit(2);
	}
	return opts;
}

int
main(int argc, char **argv)
{
	Options		opts = parse_options(argc, argv);
	Value	   *records = value_array();
	Value	   *output_records;
	Buf			out;
	ServerLogConfig log_config;

	verify_crc = opts.verify_crc;
	for (int i = opts.first_file_arg; i < argc; i++)
		parse_file_records(argv[i], records);
	buf_init(&out);
	if (opts.postgres_log)
	{
		log_config = log_config_from_options(&opts);
		render_postgres_log(&out, records, opts.format, &log_config);
	}
	else
	{
		output_records = opts.raw ? records : postgres_records(records);
		switch (opts.format)
		{
			case FMT_JSON:
				render_json_value(&out, output_records, 0, NULL);
				break;
			case FMT_YAML:
				render_yaml_obj(&out, output_records, 0);
				if (out.len > 0 && out.data[out.len - 1] == '\n')
					out.data[--out.len] = '\0';
				break;
			case FMT_XML:
				render_xml_document(&out, output_records);
				break;
			case FMT_TEXT:
				if (opts.raw)
					render_raw_text(&out, records);
				else
					render_postgres_text(&out, records);
				break;
		}
	}
	if (out.data && out.len > 0)
		fwrite(out.data, 1, out.len, stdout);
	fputc('\n', stdout);
	return 0;
}
