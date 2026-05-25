/*-------------------------------------------------------------------------
 *
 * auto_explain_z.c
 *
 *	  Low-overhead binary execution plan logger.
 *
 * IDENTIFICATION
 *	  auto_explain_z/auto_explain_z.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef USE_LZ4
#include <lz4.h>
#include <lz4frame.h>
#endif
#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "access/parallel.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "commands/dbcommands.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "foreign/fdwapi.h"
#include "jit/jit.h"
#include "libpq/libpq-be.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/params.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"

PG_MODULE_MAGIC_EXT(
					.name = "auto_explain_z",
					.version = PG_VERSION
);

#define AEZ_FILE_MAGIC			((uint32) 0x315a4541)	/* "AEZ1" */
#define AEZ_FORMAT_VERSION		10
#define AEZ_FILE_HEADER_LEN		40
#define AEZ_BYTES_TO_KILOBYTES(b) (((b) + 1023) / 1024)
#define AEZ_DEFAULT_PENDING_BUFFER_SIZE_KB 1024
#define AEZ_COMPRESSION_WORK_BUFFER_SIZE (64 * 1024)
#define AEZ_SECONDS_TO_MICROSECONDS(sec) ((int64) ((sec) * 1000000.0 + 0.5))

#define AEZ_SIZE_1				0
#define AEZ_SIZE_2				1
#define AEZ_SIZE_4				2
#define AEZ_SIZE_8				3

#define AEZ_RECORD_CTRL_LEN_MASK		0x03
#define AEZ_RECORD_CTRL_RECNO_SHIFT		2
#define AEZ_RECORD_CTRL_TS_SHIFT		4
#define AEZ_RECORD_CTRL_DUR_SHIFT		6
#define AEZ_RECORD_CTRL2_FLAGS_MASK		0x03
#define AEZ_RECORD_CTRL2_QID_SHIFT		2
#define AEZ_RECORD_CTRL2_PROFILE_SHIFT	4
#define AEZ_RECORD_CTRL2_HAS_QID		0x20

#define AEZ_TEMPLATE_CTRL_MODE_MASK		0x03
#define AEZ_TEMPLATE_CTRL_ID_SHIFT		2
#define AEZ_TEMPLATE_CTRL_HASH_SHIFT		4
#define AEZ_TEMPLATE_CTRL_HAS_METRICS	0x40
#define AEZ_TEMPLATE_CTRL_HAS_DETAILS	0x80

#define AEZ_CONTEXT_CTRL_REF			0x80

#define AEZ_STRING_NULL_CTRL			0x80

typedef enum AezCompression
{
	AEZ_COMPRESSION_NONE = 0,
	AEZ_COMPRESSION_LZ4,
	AEZ_COMPRESSION_ZSTD,
} AezCompression;

typedef enum AezProfile
{
	AEZ_PROFILE_SIMPLE = 0,
	AEZ_PROFILE_FULL,
} AezProfile;

typedef enum AezPlanRelationship
{
	AEZ_PLAN_REL_ROOT = 0,
	AEZ_PLAN_REL_OUTER,
	AEZ_PLAN_REL_INNER,
	AEZ_PLAN_REL_MEMBER,
	AEZ_PLAN_REL_SUBQUERY,
	AEZ_PLAN_REL_INITPLAN,
	AEZ_PLAN_REL_SUBPLAN,
	AEZ_PLAN_REL_CUSTOM,
} AezPlanRelationship;

typedef enum AezTemplateMode
{
	AEZ_TEMPLATE_NONE = 0,
	AEZ_TEMPLATE_DEFINE,
	AEZ_TEMPLATE_REF,
} AezTemplateMode;

#define AEZ_QUERY_FLAG_ANALYZE		0x00000001
#define AEZ_QUERY_FLAG_VERBOSE		0x00000002
#define AEZ_QUERY_FLAG_COSTS		0x00000004
#define AEZ_QUERY_FLAG_TIMING		0x00000008
#define AEZ_QUERY_FLAG_BUFFERS		0x00000010
#define AEZ_QUERY_FLAG_WAL			0x00000020
#define AEZ_QUERY_FLAG_QUERY_TEXT	0x00000040
#define AEZ_QUERY_FLAG_PARAMS		0x00000080

#define AEZ_NODE_FLAG_PARALLEL_AWARE	0x0001
#define AEZ_NODE_FLAG_ASYNC_CAPABLE		0x0002
#define AEZ_NODE_FLAG_HAS_ACTUAL		0x0004
#define AEZ_NODE_FLAG_NEVER_EXECUTED	0x0008
#define AEZ_NODE_FLAG_HAS_BUFFERS		0x0010
#define AEZ_NODE_FLAG_HAS_WAL			0x0020
#define AEZ_NODE_FLAG_DISABLED			0x0040

#define AEZ_EXTRA_MODIFY_OPERATION		1
#define AEZ_EXTRA_FOREIGN_OPERATION		2
#define AEZ_EXTRA_JOIN_TYPE				3
#define AEZ_EXTRA_AGG					4
#define AEZ_EXTRA_SETOP					5
#define AEZ_EXTRA_INDEX_SCAN_DIRECTION	6

#define AEZ_OBJ_RELATION	0x01
#define AEZ_OBJ_INDEX		0x02

typedef enum AezDetailCode
{
	AEZ_DETAIL_OUTPUT = 1,
	AEZ_DETAIL_FILTER,
	AEZ_DETAIL_INDEX_COND,
	AEZ_DETAIL_INDEX_ORDER_BY,
	AEZ_DETAIL_RECHECK_COND,
	AEZ_DETAIL_TID_COND,
	AEZ_DETAIL_JOIN_FILTER,
	AEZ_DETAIL_HASH_COND,
	AEZ_DETAIL_MERGE_COND,
	AEZ_DETAIL_SORT_KEY,
	AEZ_DETAIL_PRESORTED_KEY,
	AEZ_DETAIL_GROUP_KEY,
	AEZ_DETAIL_HASH_KEY,
	AEZ_DETAIL_FUNCTION_CALL,
	AEZ_DETAIL_TABLE_FUNCTION_CALL,
	AEZ_DETAIL_ONE_TIME_FILTER,
	AEZ_DETAIL_RUN_CONDITION,
	AEZ_DETAIL_HEAP_FETCHES,
	AEZ_DETAIL_WORKERS_PLANNED,
	AEZ_DETAIL_WORKERS_LAUNCHED,
	AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
	AEZ_DETAIL_ROWS_REMOVED_BY_JOIN_FILTER,
	AEZ_DETAIL_ROWS_REMOVED_BY_INDEX_RECHECK,
	AEZ_DETAIL_INNER_UNIQUE,
	AEZ_DETAIL_SORT_INFO,
	AEZ_DETAIL_INCREMENTAL_SORT_GROUP,
	AEZ_DETAIL_HASH_INFO,
	AEZ_DETAIL_STORAGE_INFO,
	AEZ_DETAIL_MEMOIZE_CACHE_KEY,
	AEZ_DETAIL_MEMOIZE_CACHE_MODE,
	AEZ_DETAIL_MEMOIZE_STATS,
	AEZ_DETAIL_HASHAGG_PLANNED_PARTITIONS,
	AEZ_DETAIL_HASHAGG_STATS,
	AEZ_DETAIL_INDEX_SEARCHES,
	AEZ_DETAIL_BITMAP_HEAP_BLOCKS,
	AEZ_DETAIL_SUBPLANS_REMOVED,
	AEZ_DETAIL_CONFLICT_RESOLUTION,
	AEZ_DETAIL_CONFLICT_ARBITER_INDEX,
	AEZ_DETAIL_CONFLICT_FILTER,
	AEZ_DETAIL_ROWS_REMOVED_BY_CONFLICT_FILTER,
	AEZ_DETAIL_CONFLICT_TUPLES,
	AEZ_DETAIL_MERGE_TUPLES,
	AEZ_DETAIL_TRIGGER,
	AEZ_DETAIL_JIT_SUMMARY,
	AEZ_DETAIL_EXTENSION_TEXT,
	AEZ_DETAIL_MERGE_ACTION,
	AEZ_DETAIL_PLANNING_BUFFERS,
	AEZ_DETAIL_CUSTOM_PLAN_PROVIDER,
	AEZ_DETAIL_SINGLE_COPY,
} AezDetailCode;

typedef enum AezDetailType
{
	AEZ_DETAIL_TYPE_STRING = 1,
	AEZ_DETAIL_TYPE_STRING_LIST,
	AEZ_DETAIL_TYPE_DOUBLE,
	AEZ_DETAIL_TYPE_INT64,
	AEZ_DETAIL_TYPE_UINT64,
	AEZ_DETAIL_TYPE_BOOL,
} AezDetailType;

typedef struct AezSerializeState
{
	QueryDesc  *queryDesc;
	List	   *rtable;
	List	   *rtable_names;
	List	   *deparse_cxt;
	int			rtable_size;
	bool		verbose;
	bool		analyze;
	bool		deparse_ready;
} AezSerializeState;

typedef struct AezTemplateKey
{
	uint64		query_id;
	uint64		shape_hash;
	uint32		flags;
	uint16		profile;
	uint16		padding;
} AezTemplateKey;

typedef struct AezTemplateEntry
{
	AezTemplateKey key;
	uint32		template_id;
	uint32		plan_bytes;
} AezTemplateEntry;

typedef struct AezQueryTemplateKey
{
	uint64		query_id;
	uintptr_t	plannedstmt;
	uint32		flags;
	uint16		profile;
	uint16		padding;
} AezQueryTemplateKey;

typedef struct AezQueryTemplateEntry
{
	AezQueryTemplateKey key;
	uint64		shape_hash;
	uint64		ref_count;
	uint32		template_id;
	uint32		plan_bytes;
} AezQueryTemplateEntry;

typedef struct AezPlanningEntry
{
	PlannedStmt *plannedstmt;
	BufferUsage usage;
} AezPlanningEntry;

typedef struct AezLogFile
{
	char	   *path;
	uint64		size;
	TimestampTz mtime;
	bool		is_current;
} AezLogFile;

/* GUC variables */
static int	auto_explain_z_log_min_duration = -1;	/* msec or -1 */
static int	auto_explain_z_log_parameter_max_length = -1;	/* bytes or -1 */
static int	auto_explain_z_query_text_max_length = -1;	/* bytes or -1 */
static int	auto_explain_z_max_templates = 4096;
static int	auto_explain_z_template_min_plan_bytes = 0;
static int	auto_explain_z_retention_max_files = 0;
static int	auto_explain_z_retention_max_size = 0;	/* kB, 0 disables */
static int	auto_explain_z_retention_cleanup_interval = 60000;	/* msec */
static int	auto_explain_z_pending_buffer_size = AEZ_DEFAULT_PENDING_BUFFER_SIZE_KB;	/* kB */
static bool auto_explain_z_log_analyze = false;
static bool auto_explain_z_log_verbose = false;
static bool auto_explain_z_log_buffers = false;
static bool auto_explain_z_log_wal = false;
static bool auto_explain_z_log_timing = true;
static bool auto_explain_z_log_nested_statements = false;
static bool auto_explain_z_log_query_text = true;
static bool auto_explain_z_report_writer_errors = false;
static bool auto_explain_z_template_cache = true;
static bool auto_explain_z_template_fast_path = true;
static bool auto_explain_z_template_omit_query_text = true;
static double auto_explain_z_sample_rate = 1;
static char *auto_explain_z_directory = "auto_explain_z";
static char *auto_explain_z_file_prefix = "auto_explain_z";
static int	auto_explain_z_max_file_size = 1024 * 1024;	/* kB */
static int	auto_explain_z_profile = AEZ_PROFILE_SIMPLE;
static int	auto_explain_z_template_fast_path_recheck = 0;
#ifdef USE_LZ4
static int	auto_explain_z_compression = AEZ_COMPRESSION_LZ4;
#else
static int	auto_explain_z_compression = AEZ_COMPRESSION_NONE;
#endif
static int	auto_explain_z_zstd_level = 1;

static const struct config_enum_entry compression_options[] = {
	{"none", AEZ_COMPRESSION_NONE, false},
#ifdef USE_LZ4
	{"lz4", AEZ_COMPRESSION_LZ4, false},
#endif
#ifdef USE_ZSTD
	{"zstd", AEZ_COMPRESSION_ZSTD, false},
#endif
	{NULL, 0, false}
};

static const struct config_enum_entry profile_options[] = {
	{"simple", AEZ_PROFILE_SIMPLE, false},
	{"full", AEZ_PROFILE_FULL, false},
	{NULL, 0, false}
};

/* Current nesting depth of ExecutorRun calls */
static int	nesting_level = 0;

/* Is the current top-level query to be sampled? */
static bool current_query_sampled = false;

#define auto_explain_z_enabled() \
	(auto_explain_z_log_min_duration >= 0 && \
	 (nesting_level == 0 || auto_explain_z_log_nested_statements) && \
	 current_query_sampled)

/* Saved hook values */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static planner_hook_type prev_planner = NULL;

/* Per-backend writer state. */
static int	aez_fd = -1;
static uint64 aez_file_index = 0;
static uint64 aez_file_bytes = 0;
static uint64 aez_file_uncompressed_bytes = 0;
static int	aez_file_compression = AEZ_COMPRESSION_NONE;
static uint64 aez_record_no = 0;
static bool aez_writer_failed = false;
static char *aez_current_path = NULL;
static char *aez_current_directory = NULL;
static char *aez_current_prefix = NULL;
static char *aez_current_directory_guc = NULL;
static char *aez_current_prefix_guc = NULL;
static StringInfoData aez_pending;
static bool aez_pending_initialized = false;
static StringInfoData aez_payload_buf;
static bool aez_payload_buf_initialized = false;
static StringInfoData aez_record_buf;
static bool aez_record_buf_initialized = false;
static StringInfoData aez_record_body_buf;
static bool aez_record_body_buf_initialized = false;
static StringInfoData aez_context_cache;
static bool aez_context_cache_initialized = false;
static BackendType aez_context_backend_type = B_INVALID;
static Oid	aez_context_database_id = InvalidOid;
static Oid	aez_context_user_id = InvalidOid;
static char *aez_context_appname = NULL;
static QueryDesc *aez_light_query_desc = NULL;
static TimestampTz aez_light_start_ts = 0;
static TimestampTz aez_last_cleanup_ts = 0;
static HTAB *aez_template_hash = NULL;
static HTAB *aez_query_template_hash = NULL;
static HTAB *aez_planning_hash = NULL;
static uint32 aez_template_count = 0;
static uint32 aez_next_template_id = 1;

#ifdef USE_LZ4
static LZ4F_compressionContext_t aez_lz4f_cctx = NULL;
#endif
#ifdef USE_ZSTD
static ZSTD_CStream *aez_zstd_cstream = NULL;
static char *aez_zstd_out = NULL;
static size_t aez_zstd_out_size = 0;
#endif

static void aez_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void aez_ExecutorRun(QueryDesc *queryDesc,
							ScanDirection direction,
							uint64 count);
static void aez_ExecutorFinish(QueryDesc *queryDesc);
static void aez_ExecutorEnd(QueryDesc *queryDesc);
static PlannedStmt *aez_planner(Query *parse, const char *query_string,
								int cursorOptions,
								ParamListInfo boundParams);

static void aez_proc_exit(int code, Datum arg);
static bool aez_write_record(StringInfo record);
static bool aez_file_preflight_needed(void);
static bool aez_open_file(uint64 needed);
static void aez_close_file(void);
static int	aez_select_compression(void);
static bool aez_start_compression(void);
static bool aez_finish_compression(void);
static void aez_init_pending_buffer(void);
static bool aez_flush_pending(void);
static bool aez_write_file_data(const char *data, size_t len);
static void aez_cleanup_logs(bool force);
static int	aez_log_file_cmp(const void *a, const void *b);
static bool aez_log_filename_matches(const char *name);
static bool aez_has_suffix(const char *name, const char *suffix);
static bool aez_write_all(int fd, const char *data, size_t len);
static void aez_writer_warning(const char *message);
static void aez_reset_top_stringinfo(StringInfoData *buf, bool *initialized);
static void aez_build_file_header(StringInfo buf);
static uint32 aez_build_payload(StringInfo buf, QueryDesc *queryDesc,
								uint64 *record_query_id,
								BufferUsage *planning_usage);
static bool aez_try_write_fast_template_ref(QueryDesc *queryDesc,
											uint64 query_id,
											int64 duration_us,
											TimestampTz record_ts);
static void aez_build_record(StringInfo record, StringInfo payload,
							  uint32 payload_flags, uint64 query_id,
							  int64 duration_us, TimestampTz record_ts);
static void aez_append_log_context(StringInfo buf);
static bool aez_context_can_reference(void);
static bool aez_query_may_have_details(QueryDesc *queryDesc,
									   uint32 payload_flags,
									   BufferUsage *planning_usage);
static bool aez_serialize_query_details(StringInfo buf, QueryDesc *queryDesc,
										uint32 payload_flags,
										BufferUsage *planning_usage);
static void aez_remember_planning_usage(PlannedStmt *plannedstmt,
										BufferUsage *usage);
static bool aez_take_planning_usage(PlannedStmt *plannedstmt,
									BufferUsage *usage);
static bool aez_buffer_usage_any(const BufferUsage *usage);
static void aez_serialize_planning_buffers(StringInfo details,
										   int *detail_count,
										   BufferUsage *usage);
static char *aez_build_param_log_string(ParamListInfo params);
static bool aez_try_build_fast_param_log_string(ParamListInfo params,
												char *dst, Size dstlen);
static bool aez_append_fast_param_value(StringInfo buf, ParamExternData *param,
										int maxlen);
static bool aez_fast_param_value(ParamExternData *param, char *value,
								 Size valuesize);
static void aez_append_ascii_quoted(StringInfo buf, const char *str,
									int maxlen);
static void aez_serialize_triggers(StringInfo details, int *detail_count,
								   QueryDesc *queryDesc, bool timing);
static void aez_serialize_trigger_list(StringInfo details, int *detail_count,
									   List *result_rels,
									   bool show_relname, bool timing);
static void aez_serialize_jit_summary(StringInfo details, int *detail_count,
									  QueryDesc *queryDesc, uint32 payload_flags);
static uint64 aez_plan_shape_hash(AezSerializeState *state,
								  PlanState *planstate,
								  AezPlanRelationship relationship);
static uint64 aez_hash_plan_shape_node(AezSerializeState *state,
									   uint64 hash, PlanState *planstate,
									   AezPlanRelationship relationship);
static uint64 aez_hash_plan_shape_children(AezSerializeState *state,
										   uint64 hash,
										   PlanState *planstate);
static uint64 aez_hash_plan_shape_members(AezSerializeState *state,
										  uint64 hash,
										  PlanState **planstates,
										  int nplans,
										  AezPlanRelationship relationship);
static uint64 aez_hash_plan_shape_subplans(AezSerializeState *state,
										   uint64 hash, List *plans,
										   AezPlanRelationship relationship);
static uint64 aez_hash_plan_static_fields(AezSerializeState *state,
										  uint64 hash, Plan *plan);
static uint64 aez_hash_node_string(uint64 hash, const void *node);
static uint64 aez_hash_bytes_value(uint64 hash, const void *data, Size len);
static uint64 aez_hash_u32(uint64 hash, uint32 value);
static uint64 aez_hash_u64(uint64 hash, uint64 value);
static uint64 aez_hash_bool(uint64 hash, bool value);
static int64 aez_explain_time_microseconds(double seconds);
static void aez_reset_templates(void);
static void aez_invalidate_context_cache(void);
static AezTemplateEntry *aez_find_template(AezTemplateKey *key);
static AezTemplateEntry *aez_add_template(AezTemplateKey *key,
										  uint32 plan_bytes);
static AezQueryTemplateEntry *aez_find_query_template(AezQueryTemplateKey *key);
static void aez_remember_query_template(AezQueryTemplateKey *key,
										AezTemplateEntry *template_entry);
static bool aez_template_ref_needs_metrics(uint32 payload_flags);
static bool aez_template_ref_can_omit_metrics(QueryDesc *queryDesc,
											  uint32 payload_flags);
static bool aez_prescan_node(PlanState *planstate, Bitmapset **rels_used);
static void aez_init_serialize_state(AezSerializeState *state,
									 QueryDesc *queryDesc,
									 uint32 payload_flags);
static void aez_init_deparse_state(AezSerializeState *state);
static bool aez_plan_is_disabled(Plan *plan);
static void aez_serialize_plan_node(StringInfo buf, AezSerializeState *state,
									PlanState *planstate,
									AezPlanRelationship relationship,
									List *ancestors);
static int	aez_count_plan_children(PlanState *planstate);
static void aez_serialize_plan_children(StringInfo buf, AezSerializeState *state,
										PlanState *planstate,
										List *ancestors);
static void aez_serialize_plan_member_nodes(StringInfo buf, AezSerializeState *state,
											PlanState **planstates, int nplans,
											AezPlanRelationship relationship,
											List *ancestors);
static void aez_serialize_subplans(StringInfo buf, AezSerializeState *state,
								   List *plans,
								   AezPlanRelationship relationship,
								   List *ancestors);
static void aez_append_plan_identity(StringInfo buf, AezSerializeState *state,
									 Plan *plan);
static void aez_serialize_plan_metrics(StringInfo buf,
									   AezSerializeState *state,
									   PlanState *planstate,
									   List *ancestors);
static void aez_serialize_plan_metric_children(StringInfo buf,
											   AezSerializeState *state,
											   PlanState *planstate,
											   List *ancestors);
static void aez_serialize_dynamic_node_details(StringInfo details,
											   int *detail_count,
											   AezSerializeState *state,
											   PlanState *planstate);
static void aez_serialize_structural_node_details(StringInfo details,
												  int *detail_count,
												  PlanState *planstate);
static void aez_serialize_runtime_node_details(StringInfo details,
											   int *detail_count,
											   AezSerializeState *state,
											   PlanState *planstate,
											   List *ancestors,
											   bool dynamic_only);
static void aez_detail_sort_info(StringInfo details, int *detail_count,
								 AezSerializeState *state,
								 SortState *sortstate);
static void aez_detail_incremental_sort_info(StringInfo details,
											 int *detail_count,
											 AezSerializeState *state,
											 IncrementalSortState *incrsortstate);
static void aez_detail_incremental_sort_group(StringInfo details,
											  int *detail_count,
											  int worker,
											  IncrementalSortGroupInfo *groupInfo,
											  const char *label);
static void aez_detail_hash_info(StringInfo details, int *detail_count,
								 HashState *hashstate);
static void aez_detail_storage_info(StringInfo details, int *detail_count,
									AezDetailCode code,
									Tuplestorestate *tupstore);
static void aez_detail_memoize_info(StringInfo details, int *detail_count,
									AezSerializeState *state,
									MemoizeState *mstate,
									List *ancestors,
									bool dynamic_only);
static void aez_detail_hashagg_info(StringInfo details, int *detail_count,
									AezSerializeState *state,
									AggState *aggstate,
									bool dynamic_only);
static void aez_detail_index_searches(StringInfo details, int *detail_count,
									  AezSerializeState *state,
									  PlanState *planstate);
static void aez_detail_bitmap_heap_blocks(StringInfo details, int *detail_count,
										  AezSerializeState *state,
										  BitmapHeapScanState *planstate);
static void aez_detail_modifytable_info(StringInfo details, int *detail_count,
										AezSerializeState *state,
										ModifyTableState *mtstate,
										List *ancestors,
										bool dynamic_only);
static void aez_detail_merge_actions(StringInfo details, int *detail_count,
									 AezSerializeState *state,
									 ModifyTableState *mtstate,
									 List *ancestors);
static char *aez_deparse_merge_action(AezSerializeState *state,
									  ModifyTableState *mtstate,
									  ResultRelInfo *resultRelInfo,
									  MergeAction *action,
									  List *ancestors);
static const char *aez_merge_match_name(MergeMatchKind matchKind);
static const char *aez_command_name(CmdType commandType);
static void aez_serialize_node_details(StringInfo details, int *detail_count,
									   AezSerializeState *state,
									   PlanState *planstate,
									   List *ancestors);
static void aez_serialize_extension_explain_text(StringInfo details,
												 int *detail_count,
												 AezSerializeState *state,
												 PlanState *planstate,
												 List *ancestors);
static bool aez_node_has_extension_explain(PlanState *planstate);
static bool aez_plan_tree_has_extension_explain(PlanState *planstate);
static bool aez_plan_members_have_extension_explain(PlanState **planstates,
													int nplans);
static bool aez_subplans_have_extension_explain(List *plans);
static ExplainState *aez_new_text_explain_state(AezSerializeState *state);
static void aez_capture_foreignscan_explain(ExplainState *es,
											ForeignScanState *fsstate);
static void aez_capture_foreignmodify_explain(ExplainState *es,
											  ModifyTableState *mtstate);
static void aez_emit_explain_text_detail(StringInfo details,
										 int *detail_count,
										 ExplainState *es);
static void aez_detail_string(StringInfo details, int *detail_count,
							  AezDetailCode code, const char *value);
static void aez_detail_double(StringInfo details, int *detail_count,
							  AezDetailCode code, double value);
static void aez_detail_i64(StringInfo details, int *detail_count,
						   AezDetailCode code, int64 value);
static void aez_detail_u64(StringInfo details, int *detail_count,
						   AezDetailCode code, uint64 value);
static void aez_detail_bool(StringInfo details, int *detail_count,
							AezDetailCode code, bool value);
static void aez_detail_string_list(StringInfo details, int *detail_count,
								   AezDetailCode code, List *values);
static void aez_detail_cstring_array(StringInfo details, int *detail_count,
									 AezDetailCode code, int nvalues,
									 const char **values);
static void aez_detail_qual(StringInfo details, int *detail_count,
							AezSerializeState *state,
							PlanState *planstate, List *ancestors,
							List *qual, AezDetailCode code, bool useprefix);
static void aez_detail_expr(StringInfo details, int *detail_count,
							AezSerializeState *state,
							PlanState *planstate, List *ancestors,
							Node *node, AezDetailCode code, bool useprefix);
static void aez_detail_targetlist(StringInfo details, int *detail_count,
								  AezSerializeState *state,
								  PlanState *planstate, List *ancestors);
static void aez_detail_sort_group_keys(StringInfo details, int *detail_count,
									   AezSerializeState *state,
									   PlanState *planstate, List *ancestors,
									   AezDetailCode code,
									   int nkeys, int nPresortedKeys,
									   AttrNumber *keycols,
									   Oid *sortOperators,
									   Oid *collations,
									   bool *nullsFirst);
static void aez_append_sortorder_options(StringInfo buf, Node *sortexpr,
										 Oid sortOperator, Oid collation,
										 bool nullsFirst);
static void aez_detail_instrumentation_count(StringInfo details,
											 int *detail_count,
											 AezSerializeState *state,
											 PlanState *planstate,
											 AezDetailCode code, int which);
static void aez_put_u8(StringInfo buf, uint8 value);
static void aez_put_u16(StringInfo buf, uint16 value);
static void aez_put_u32(StringInfo buf, uint32 value);
static void aez_put_u64(StringInfo buf, uint64 value);
static void aez_put_i64(StringInfo buf, int64 value);
static void aez_put_double(StringInfo buf, double value);
static uint8 aez_uint_size_code(uint64 value);
static uint8 aez_int_size_code(int64 value);
static void aez_put_uint_sized(StringInfo buf, uint64 value, uint8 code);
static void aez_put_int_sized(StringInfo buf, int64 value, uint8 code);
static void aez_put_string(StringInfo buf, const char *value, int maxlen);

void
_PG_init(void)
{
	DefineCustomIntVariable("auto_explain_z.log_min_duration",
							"Sets the minimum execution time above which plans will be written to auto_explain_z binary logs.",
							"-1 disables binary plan logging. 0 means log all plans.",
							&auto_explain_z_log_min_duration,
							-1,
							-1, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	DefineCustomRealVariable("auto_explain_z.sample_rate",
							 "Fraction of queries to process.",
							 NULL,
							 &auto_explain_z_sample_rate,
							 1.0,
							 0.0,
							 1.0,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.log_analyze",
							 "Collect per-node execution statistics.",
							 NULL,
							 &auto_explain_z_log_analyze,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.log_verbose",
							 "Include verbose EXPLAIN fields where the selected binary profile supports them.",
							 NULL,
							 &auto_explain_z_log_verbose,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.log_buffers",
							 "Collect per-node buffer usage.",
							 "This has no effect unless log_analyze is also set.",
							 &auto_explain_z_log_buffers,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.log_wal",
							 "Collect per-node WAL usage.",
							 "This has no effect unless log_analyze is also set.",
							 &auto_explain_z_log_wal,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.log_timing",
							 "Collect timing data, not just row counts.",
							 NULL,
							 &auto_explain_z_log_timing,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.log_nested_statements",
							 "Log nested statements.",
							 NULL,
							 &auto_explain_z_log_nested_statements,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.log_query_text",
							 "Store query text in binary plan records.",
							 NULL,
							 &auto_explain_z_log_query_text,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.report_writer_errors",
							 "Report auto_explain_z writer failures through the regular PostgreSQL log.",
							 "Disabled by default so plan logging does not add regular-log traffic.",
							 &auto_explain_z_report_writer_errors,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.template_cache",
							 "Use a bounded per-backend plan template dictionary.",
							 "When enabled, repeated plan shapes can be logged as template references plus per-execution metrics.",
							 &auto_explain_z_template_cache,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.template_fast_path",
							 "Use query-id template refs without hashing the plan on every execution.",
							 "Applies only when a template reference does not need per-execution plan metrics.",
							 &auto_explain_z_template_fast_path,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain_z.template_omit_query_text",
							 "Omit query text from template-reference records.",
							 "The defining template record still stores query text when log_query_text is enabled.",
							 &auto_explain_z_template_omit_query_text,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("auto_explain_z.template_fast_path_recheck",
							"Executions between plan-shape rechecks for query-id template fast path.",
							"0 disables periodic rechecks; the planned statement pointer is still part of the fast-path key.",
							&auto_explain_z_template_fast_path_recheck,
							0,
							0, INT_MAX,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("auto_explain_z.query_text_max_length",
							"Sets the maximum length of query text to store.",
							"-1 stores query text in full.",
							&auto_explain_z_query_text_max_length,
							-1,
							-1, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_BYTE,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("auto_explain_z.max_templates",
							"Maximum number of plan templates to keep per backend per binary log file.",
							"Once the limit is reached, new plan shapes are written as full standalone records.",
							&auto_explain_z_max_templates,
							4096,
							0, INT_MAX,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("auto_explain_z.template_min_plan_bytes",
							"Minimum full serialized plan size eligible for template caching.",
							"0 allows even small repeated plans to use template references.",
							&auto_explain_z_template_min_plan_bytes,
							0,
							0, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_BYTE,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("auto_explain_z.log_parameter_max_length",
							"Sets the maximum length of query parameter values to store.",
							"-1 stores values in full. 0 disables parameter logging.",
							&auto_explain_z_log_parameter_max_length,
							-1,
							-1, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_BYTE,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("auto_explain_z.directory",
							   "Directory for auto_explain_z binary log files.",
							   "Relative paths are resolved below the PostgreSQL data directory.",
							   &auto_explain_z_directory,
							   "auto_explain_z",
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("auto_explain_z.file_prefix",
							   "Prefix for auto_explain_z binary log file names.",
							   NULL,
							   &auto_explain_z_file_prefix,
							   "auto_explain_z",
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomIntVariable("auto_explain_z.max_file_size",
							"Maximum size of each per-backend binary log file.",
							"0 disables size-based rotation.",
							&auto_explain_z_max_file_size,
							1024 * 1024,
							0, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_KB,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("auto_explain_z.pending_buffer_size",
							"Amount of serialized record data to buffer before writing or compressing.",
							"0 disables pending buffering and flushes every record immediately.",
							&auto_explain_z_pending_buffer_size,
							AEZ_DEFAULT_PENDING_BUFFER_SIZE_KB,
							0, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_KB,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("auto_explain_z.retention_max_files",
							"Maximum number of auto_explain_z binary log files to keep.",
							"0 disables file-count retention cleanup.",
							&auto_explain_z_retention_max_files,
							0,
							0, INT_MAX,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("auto_explain_z.retention_max_size",
							"Maximum total size of auto_explain_z binary log files to keep.",
							"0 disables size-based retention cleanup.",
							&auto_explain_z_retention_max_size,
							0,
							0, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_KB,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("auto_explain_z.retention_cleanup_interval",
							"Minimum interval between auto_explain_z retention cleanup scans.",
							"0 runs cleanup whenever a backend opens or rotates a binary log file.",
							&auto_explain_z_retention_cleanup_interval,
							60000,
							0, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("auto_explain_z.compression",
							 "Compression method for binary plan log files.",
							 NULL,
							 &auto_explain_z_compression,
							 auto_explain_z_compression,
							 compression_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("auto_explain_z.zstd_level",
							"Compression level to use when compression is zstd.",
							NULL,
							&auto_explain_z_zstd_level,
							1,
							1, 22,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("auto_explain_z.profile",
							 "Binary serialization profile.",
							 "simple stores the compact baseline; full adds EXPLAIN detail fields.",
							 &auto_explain_z_profile,
							 AEZ_PROFILE_SIMPLE,
							 profile_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("auto_explain_z");

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = aez_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = aez_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = aez_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = aez_ExecutorEnd;
	prev_planner = planner_hook;
	planner_hook = aez_planner;

	on_proc_exit(aez_proc_exit, 0);
}

static void
aez_remember_planning_usage(PlannedStmt *plannedstmt, BufferUsage *usage)
{
	HASHCTL		ctl;
	AezPlanningEntry *entry;
	bool		found;

	if (plannedstmt == NULL || usage == NULL || !aez_buffer_usage_any(usage))
		return;

	if (aez_planning_hash == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(PlannedStmt *);
		ctl.entrysize = sizeof(AezPlanningEntry);
		ctl.hcxt = TopMemoryContext;
		aez_planning_hash =
			hash_create("auto_explain_z planning buffer usage",
						32, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	entry = (AezPlanningEntry *) hash_search(aez_planning_hash,
											 &plannedstmt, HASH_ENTER,
											 &found);
	entry->plannedstmt = plannedstmt;
	entry->usage = *usage;
}

static bool
aez_take_planning_usage(PlannedStmt *plannedstmt, BufferUsage *usage)
{
	AezPlanningEntry *entry;
	bool		found;

	if (aez_planning_hash == NULL || plannedstmt == NULL || usage == NULL)
		return false;

	entry = (AezPlanningEntry *) hash_search(aez_planning_hash,
											 &plannedstmt, HASH_FIND,
											 &found);
	if (!found)
		return false;

	*usage = entry->usage;
	(void) hash_search(aez_planning_hash, &plannedstmt, HASH_REMOVE, NULL);
	return true;
}

static bool
aez_buffer_usage_any(const BufferUsage *usage)
{
	return usage->shared_blks_hit > 0 ||
		usage->shared_blks_read > 0 ||
		usage->shared_blks_dirtied > 0 ||
		usage->shared_blks_written > 0 ||
		usage->local_blks_hit > 0 ||
		usage->local_blks_read > 0 ||
		usage->local_blks_dirtied > 0 ||
		usage->local_blks_written > 0 ||
		usage->temp_blks_read > 0 ||
		usage->temp_blks_written > 0 ||
		!INSTR_TIME_IS_ZERO(usage->shared_blk_read_time) ||
		!INSTR_TIME_IS_ZERO(usage->shared_blk_write_time) ||
		!INSTR_TIME_IS_ZERO(usage->local_blk_read_time) ||
		!INSTR_TIME_IS_ZERO(usage->local_blk_write_time) ||
		!INSTR_TIME_IS_ZERO(usage->temp_blk_read_time) ||
		!INSTR_TIME_IS_ZERO(usage->temp_blk_write_time);
}

static PlannedStmt *
aez_planner(Query *parse, const char *query_string, int cursorOptions,
			ParamListInfo boundParams)
{
	BufferUsage start_usage;
	BufferUsage usage;
	PlannedStmt *plannedstmt;

	start_usage = pgBufferUsage;

	if (prev_planner)
		plannedstmt = prev_planner(parse, query_string, cursorOptions,
								   boundParams);
	else
		plannedstmt = standard_planner(parse, query_string, cursorOptions,
									   boundParams);

	memset(&usage, 0, sizeof(usage));
	BufferUsageAccumDiff(&usage, &pgBufferUsage, &start_usage);
	if (auto_explain_z_log_min_duration >= 0 && !IsParallelWorker())
		aez_remember_planning_usage(plannedstmt, &usage);

	return plannedstmt;
}

static void
aez_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (nesting_level == 0)
	{
		if (auto_explain_z_log_min_duration >= 0 && !IsParallelWorker())
		{
			if (auto_explain_z_sample_rate >= 1.0)
				current_query_sampled = true;
			else if (auto_explain_z_sample_rate <= 0.0)
				current_query_sampled = false;
			else
				current_query_sampled =
					(pg_prng_double(&pg_global_prng_state) <
					 auto_explain_z_sample_rate);
		}
		else
			current_query_sampled = false;
	}

	if (auto_explain_z_enabled())
	{
		if (auto_explain_z_log_analyze &&
			(eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
		{
			if (auto_explain_z_log_timing)
				queryDesc->instrument_options |= INSTRUMENT_TIMER;
			else
				queryDesc->instrument_options |= INSTRUMENT_ROWS;
			if (auto_explain_z_log_buffers)
				queryDesc->instrument_options |= INSTRUMENT_BUFFERS;
			if (auto_explain_z_log_wal)
				queryDesc->instrument_options |= INSTRUMENT_WAL;
		}
	}

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (auto_explain_z_enabled() && !auto_explain_z_log_analyze)
	{
		aez_light_query_desc = queryDesc;
		aez_light_start_ts = GetCurrentTimestamp();
	}
	else if (auto_explain_z_enabled() && queryDesc->totaltime == NULL)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
		queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
		MemoryContextSwitchTo(oldcxt);
	}
}

static void
aez_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

static void
aez_ExecutorFinish(QueryDesc *queryDesc)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

static void
aez_ExecutorEnd(QueryDesc *queryDesc)
{
	if (!auto_explain_z_enabled() && queryDesc->plannedstmt != NULL &&
		aez_planning_hash != NULL)
	{
		BufferUsage discarded;

		(void) aez_take_planning_usage(queryDesc->plannedstmt, &discarded);
	}

	if (auto_explain_z_enabled() &&
		(queryDesc->totaltime || aez_light_query_desc == queryDesc))
	{
		MemoryContext oldcxt;
		double		msec;
		int64		duration_us;
		TimestampTz record_ts;
		BufferUsage planning_usage;
		BufferUsage *planning_usage_ptr = NULL;

		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);

		if (aez_light_query_desc == queryDesc)
		{
			record_ts = GetCurrentTimestamp();
			duration_us = record_ts - aez_light_start_ts;
			msec = duration_us / 1000.0;
		}
		else
		{
			InstrEndLoop(queryDesc->totaltime);
			duration_us = AEZ_SECONDS_TO_MICROSECONDS(queryDesc->totaltime->total);
			record_ts = GetCurrentTimestamp();
			msec = queryDesc->totaltime->total * 1000.0;
		}

		if (queryDesc->plannedstmt &&
			aez_take_planning_usage(queryDesc->plannedstmt, &planning_usage))
			planning_usage_ptr = &planning_usage;

		if (msec >= auto_explain_z_log_min_duration && !aez_writer_failed)
		{
			uint64		query_id = 0;
			uint32		payload_flags;

			if (queryDesc->plannedstmt)
				query_id = queryDesc->plannedstmt->queryId;

			if (aez_file_preflight_needed() && !aez_open_file(0))
				goto logged;

			if (aez_try_write_fast_template_ref(queryDesc, query_id,
												duration_us, record_ts))
				goto logged;

			aez_reset_top_stringinfo(&aez_payload_buf,
									 &aez_payload_buf_initialized);
			aez_reset_top_stringinfo(&aez_record_buf,
									 &aez_record_buf_initialized);

			payload_flags = aez_build_payload(&aez_payload_buf, queryDesc,
											  &query_id,
											  planning_usage_ptr);
			aez_build_record(&aez_record_buf, &aez_payload_buf,
							 payload_flags, query_id,
							 duration_us, record_ts);
			(void) aez_write_record(&aez_record_buf);
		}

logged:
		if (aez_light_query_desc == queryDesc)
		{
			aez_light_query_desc = NULL;
			aez_light_start_ts = 0;
		}
		MemoryContextSwitchTo(oldcxt);
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

static void
aez_proc_exit(int code, Datum arg)
{
	aez_close_file();
	aez_reset_templates();
}

static bool
aez_try_write_fast_template_ref(QueryDesc *queryDesc, uint64 query_id,
								int64 duration_us, TimestampTz record_ts)
{
	uint32		flags = AEZ_QUERY_FLAG_COSTS;
	uint32		template_flags;
	AezQueryTemplateKey query_template_key;
	AezQueryTemplateEntry *query_template_entry;
	uint8		template_id_code;
	uint8		plan_id_code;
	uint8		template_ctrl;
	StringInfoData payload;
	char		payload_data[512];
	char		param_data[256];
	char	   *paramstr = NULL;
	bool		payload_needs_palloc = false;

	if (query_id == 0 ||
		queryDesc->plannedstmt == NULL ||
		!auto_explain_z_template_cache ||
		!auto_explain_z_template_fast_path ||
		auto_explain_z_max_templates <= 0 ||
		!auto_explain_z_template_omit_query_text ||
		!aez_context_can_reference())
		return false;

	if (auto_explain_z_log_analyze && queryDesc->instrument_options)
		flags |= AEZ_QUERY_FLAG_ANALYZE;
	if (auto_explain_z_log_verbose)
		flags |= AEZ_QUERY_FLAG_VERBOSE;
	if (auto_explain_z_log_timing)
		flags |= AEZ_QUERY_FLAG_TIMING;
	if ((flags & AEZ_QUERY_FLAG_ANALYZE) && auto_explain_z_log_buffers)
		flags |= AEZ_QUERY_FLAG_BUFFERS;
	if ((flags & AEZ_QUERY_FLAG_ANALYZE) && auto_explain_z_log_wal)
		flags |= AEZ_QUERY_FLAG_WAL;

	if (!aez_template_ref_can_omit_metrics(queryDesc, flags))
		return false;

	if (queryDesc->params != NULL &&
		queryDesc->params->numParams > 0 &&
		auto_explain_z_log_parameter_max_length != 0)
	{
		if (aez_try_build_fast_param_log_string(queryDesc->params,
												param_data,
												sizeof(param_data)))
			paramstr = param_data;
		else
			paramstr = aez_build_param_log_string(queryDesc->params);
		if (paramstr && paramstr[0] != '\0')
		{
			flags |= AEZ_QUERY_FLAG_PARAMS;
			if (strlen(paramstr) + 32 >= sizeof(payload_data))
				payload_needs_palloc = true;
		}
	}

	template_flags = flags & AEZ_QUERY_FLAG_VERBOSE;
	memset(&query_template_key, 0, sizeof(query_template_key));
	query_template_key.query_id = query_id;
	query_template_key.plannedstmt = (uintptr_t) queryDesc->plannedstmt;
	query_template_key.flags = template_flags;
	query_template_key.profile = (uint16) auto_explain_z_profile;

	query_template_entry = aez_find_query_template(&query_template_key);
	if (query_template_entry == NULL ||
		query_template_entry->plan_bytes <
		(uint32) auto_explain_z_template_min_plan_bytes ||
		(auto_explain_z_template_fast_path_recheck > 0 &&
		 query_template_entry->ref_count >=
		 (uint64) auto_explain_z_template_fast_path_recheck))
		return false;

	query_template_entry->ref_count++;

	if (payload_needs_palloc)
		initStringInfo(&payload);
	else
	{
		payload.data = payload_data;
		payload.len = 0;
		payload.maxlen = sizeof(payload_data);
		payload.cursor = 0;
		payload.data[0] = '\0';
	}

	aez_put_u8(&payload, AEZ_CONTEXT_CTRL_REF);
	template_id_code = aez_uint_size_code(query_template_entry->template_id);
	plan_id_code = aez_uint_size_code(query_template_entry->shape_hash);
	template_ctrl = AEZ_TEMPLATE_REF |
		(template_id_code << AEZ_TEMPLATE_CTRL_ID_SHIFT) |
		(plan_id_code << AEZ_TEMPLATE_CTRL_HASH_SHIFT);
	aez_put_u8(&payload, template_ctrl);
	aez_put_uint_sized(&payload, query_template_entry->template_id,
					   template_id_code);
	aez_put_uint_sized(&payload, query_template_entry->shape_hash,
					   plan_id_code);
	if (flags & AEZ_QUERY_FLAG_PARAMS)
		aez_put_string(&payload, paramstr, -1);

	aez_reset_top_stringinfo(&aez_record_buf,
							 &aez_record_buf_initialized);
	aez_build_record(&aez_record_buf, &payload, flags, 0,
					 duration_us, record_ts);
	return aez_write_record(&aez_record_buf);
}

static uint32
aez_build_payload(StringInfo buf, QueryDesc *queryDesc,
				  uint64 *record_query_id, BufferUsage *planning_usage)
{
	uint32		flags = AEZ_QUERY_FLAG_COSTS;
	uint32		template_flags = 0;
	uint64		query_id = *record_query_id;
	uint64		shape_hash = 0;
	uint32		template_id = 0;
	AezTemplateMode template_mode = AEZ_TEMPLATE_NONE;
	AezTemplateEntry *template_entry = NULL;
	AezTemplateKey template_key;
	AezQueryTemplateEntry *query_template_entry = NULL;
	AezQueryTemplateKey query_template_key;
	StringInfoData planbuf;
	StringInfoData detailsbuf;
	char		param_data[256];
	char	   *paramstr = NULL;
	AezSerializeState state;
	bool		template_has_metrics = false;
	bool		has_query_details = false;
	uint8		template_ctrl;
	uint8		template_id_code;
	uint8		plan_id_code;

	memset(&template_key, 0, sizeof(template_key));
	memset(&query_template_key, 0, sizeof(query_template_key));

	if (auto_explain_z_log_analyze && queryDesc->instrument_options)
		flags |= AEZ_QUERY_FLAG_ANALYZE;
	if (auto_explain_z_log_verbose)
		flags |= AEZ_QUERY_FLAG_VERBOSE;
	if (auto_explain_z_log_timing)
		flags |= AEZ_QUERY_FLAG_TIMING;
	if ((flags & AEZ_QUERY_FLAG_ANALYZE) && auto_explain_z_log_buffers)
		flags |= AEZ_QUERY_FLAG_BUFFERS;
	if ((flags & AEZ_QUERY_FLAG_ANALYZE) && auto_explain_z_log_wal)
		flags |= AEZ_QUERY_FLAG_WAL;
	if (auto_explain_z_log_query_text && queryDesc->sourceText)
		flags |= AEZ_QUERY_FLAG_QUERY_TEXT;

	if (queryDesc->params != NULL &&
		queryDesc->params->numParams > 0 &&
		auto_explain_z_log_parameter_max_length != 0)
	{
		if (aez_try_build_fast_param_log_string(queryDesc->params,
												param_data,
												sizeof(param_data)))
			paramstr = param_data;
		else
			paramstr = aez_build_param_log_string(queryDesc->params);
		if (paramstr && paramstr[0] != '\0')
			flags |= AEZ_QUERY_FLAG_PARAMS;
	}

	aez_init_serialize_state(&state, queryDesc, flags);

	if (auto_explain_z_template_cache &&
		auto_explain_z_max_templates > 0 &&
		query_id != 0)
	{
		template_flags = flags & AEZ_QUERY_FLAG_VERBOSE;

		query_template_key.query_id = query_id;
		query_template_key.plannedstmt = (uintptr_t) queryDesc->plannedstmt;
		query_template_key.flags = template_flags;
		query_template_key.profile = (uint16) auto_explain_z_profile;

		if (auto_explain_z_template_fast_path &&
			!aez_template_ref_needs_metrics(flags))
		{
			query_template_entry = aez_find_query_template(&query_template_key);
			if (query_template_entry &&
				query_template_entry->plan_bytes >=
				(uint32) auto_explain_z_template_min_plan_bytes &&
				(auto_explain_z_template_fast_path_recheck == 0 ||
				 query_template_entry->ref_count <
				 (uint64) auto_explain_z_template_fast_path_recheck))
			{
				template_mode = AEZ_TEMPLATE_REF;
				template_id = query_template_entry->template_id;
				shape_hash = query_template_entry->shape_hash;
				query_template_entry->ref_count++;
				if (auto_explain_z_template_omit_query_text)
					flags &= ~AEZ_QUERY_FLAG_QUERY_TEXT;
			}
		}

		if (template_mode != AEZ_TEMPLATE_REF)
		{
			shape_hash = aez_plan_shape_hash(&state, queryDesc->planstate,
											  AEZ_PLAN_REL_ROOT);
			template_key.query_id = query_id;
			template_key.shape_hash = shape_hash;
			template_key.flags = template_flags;
			template_key.profile = (uint16) auto_explain_z_profile;

			template_entry = aez_find_template(&template_key);
			if (template_entry &&
				template_entry->plan_bytes >=
				(uint32) auto_explain_z_template_min_plan_bytes)
			{
				template_mode = AEZ_TEMPLATE_REF;
				template_id = template_entry->template_id;
				template_has_metrics = aez_template_ref_needs_metrics(flags);
				aez_remember_query_template(&query_template_key,
											template_entry);
				if (auto_explain_z_template_omit_query_text)
					flags &= ~AEZ_QUERY_FLAG_QUERY_TEXT;
			}
		}
	}

	if (shape_hash == 0)
		shape_hash = aez_plan_shape_hash(&state, queryDesc->planstate,
										  AEZ_PLAN_REL_ROOT);

	if (template_mode != AEZ_TEMPLATE_REF)
	{
		initStringInfo(&planbuf);
		if (auto_explain_z_profile == AEZ_PROFILE_FULL)
			aez_init_deparse_state(&state);
		aez_serialize_plan_node(&planbuf, &state, queryDesc->planstate,
								AEZ_PLAN_REL_ROOT, NIL);

		if (shape_hash != 0 &&
			template_entry == NULL &&
			planbuf.len >= auto_explain_z_template_min_plan_bytes)
		{
			template_entry = aez_add_template(&template_key,
											  (uint32) planbuf.len);
			if (template_entry)
				{
					template_mode = AEZ_TEMPLATE_DEFINE;
					template_id = template_entry->template_id;
					aez_remember_query_template(&query_template_key,
												template_entry);
				}
			}
	}

	if (aez_query_may_have_details(queryDesc, flags, planning_usage))
	{
		initStringInfo(&detailsbuf);
		has_query_details = aez_serialize_query_details(&detailsbuf, queryDesc,
														flags,
														planning_usage);
	}

	aez_append_log_context(buf);

	template_id_code = aez_uint_size_code(template_id);
	plan_id_code = aez_uint_size_code(shape_hash);
	template_ctrl = ((uint8) template_mode & AEZ_TEMPLATE_CTRL_MODE_MASK);
	if (template_mode != AEZ_TEMPLATE_NONE)
		template_ctrl |= template_id_code << AEZ_TEMPLATE_CTRL_ID_SHIFT |
			plan_id_code << AEZ_TEMPLATE_CTRL_HASH_SHIFT;
	else
		template_ctrl |= plan_id_code << AEZ_TEMPLATE_CTRL_HASH_SHIFT;
	if (template_mode == AEZ_TEMPLATE_REF && template_has_metrics)
		template_ctrl |= AEZ_TEMPLATE_CTRL_HAS_METRICS;
	if (has_query_details)
		template_ctrl |= AEZ_TEMPLATE_CTRL_HAS_DETAILS;
	aez_put_u8(buf, template_ctrl);

	if (template_mode != AEZ_TEMPLATE_NONE)
	{
		aez_put_uint_sized(buf, template_id, template_id_code);
	}
	aez_put_uint_sized(buf, shape_hash, plan_id_code);

	if (flags & AEZ_QUERY_FLAG_QUERY_TEXT)
		aez_put_string(buf, queryDesc->sourceText,
					   auto_explain_z_query_text_max_length);

	if (flags & AEZ_QUERY_FLAG_PARAMS)
		aez_put_string(buf, paramstr, -1);

	if (template_mode == AEZ_TEMPLATE_REF && template_has_metrics)
		aez_serialize_plan_metrics(buf, &state, queryDesc->planstate, NIL);
	else if (template_mode != AEZ_TEMPLATE_REF)
		appendBinaryStringInfo(buf, planbuf.data, planbuf.len);

	if (has_query_details)
		appendBinaryStringInfo(buf, detailsbuf.data, detailsbuf.len);
	if (template_mode == AEZ_TEMPLATE_REF)
		*record_query_id = 0;
	return flags;
}

static bool
aez_template_ref_needs_metrics(uint32 payload_flags)
{
	if (payload_flags & AEZ_QUERY_FLAG_ANALYZE)
		return true;

	/*
	 * Full profile can include extension-provided EXPLAIN text and other
	 * per-execution details that are not part of the static template.
	 */
	if (auto_explain_z_profile == AEZ_PROFILE_FULL)
		return true;

	return false;
}

static bool
aez_template_ref_can_omit_metrics(QueryDesc *queryDesc, uint32 payload_flags)
{
	if (!aez_template_ref_needs_metrics(payload_flags))
		return !aez_query_may_have_details(queryDesc, payload_flags, NULL);

	if (auto_explain_z_profile != AEZ_PROFILE_FULL)
		return false;
	if (payload_flags & AEZ_QUERY_FLAG_ANALYZE)
		return false;
	if (aez_query_may_have_details(queryDesc, payload_flags, NULL))
		return false;
	if (aez_plan_tree_has_extension_explain(queryDesc->planstate))
		return false;

	return true;
}

static void
aez_build_record(StringInfo record, StringInfo payload, uint32 payload_flags,
				 uint64 query_id, int64 duration_us,
				 TimestampTz record_ts)
{
	StringInfo	body = &aez_record_body_buf;
	int64		ts_delta_us;
	int64		ts_delta;
	uint8		len_code;
	uint8		ts_code;
	uint8		duration_code;
	uint8		flags_code;
	uint8		query_id_code;
	uint8		ctrl1;
	uint8		ctrl2;

	Assert(payload->len >= 0);

	aez_record_no++;
	ts_delta_us = record_ts - MyStartTimestamp;
	ts_delta = ts_delta_us / 1000;
	ts_code = aez_int_size_code(ts_delta);
	duration_code = aez_int_size_code(duration_us);
	flags_code = aez_uint_size_code(payload_flags);
	query_id_code = aez_uint_size_code(query_id);

	aez_reset_top_stringinfo(body, &aez_record_body_buf_initialized);
	ctrl2 = flags_code |
		(query_id_code << AEZ_RECORD_CTRL2_QID_SHIFT) |
		(((uint8) auto_explain_z_profile & 0x01) <<
		 AEZ_RECORD_CTRL2_PROFILE_SHIFT);
	if (query_id != 0)
		ctrl2 |= AEZ_RECORD_CTRL2_HAS_QID;

	aez_put_u8(body, ctrl2);
	aez_put_int_sized(body, ts_delta, ts_code);
	aez_put_int_sized(body, duration_us, duration_code);
	aez_put_uint_sized(body, payload_flags, flags_code);
	if (query_id != 0)
		aez_put_uint_sized(body, query_id, query_id_code);
	appendBinaryStringInfo(body, payload->data, payload->len);

	len_code = aez_uint_size_code((uint64) body->len);
	ctrl1 = len_code |
		(ts_code << AEZ_RECORD_CTRL_TS_SHIFT) |
		(duration_code << AEZ_RECORD_CTRL_DUR_SHIFT);

	aez_put_u8(record, ctrl1);
	aez_put_uint_sized(record, (uint64) body->len, len_code);
	appendBinaryStringInfo(record, body->data, body->len);
}

static void
aez_append_log_context(StringInfo buf)
{
	const char *database_name = NULL;
	const char *authn_user = NULL;
	const char *effective_user = NULL;
	const char *remote_host = NULL;
	const char *remote_port = NULL;
	const char *appname = application_name;
	const char *backend_type = get_backend_type_for_log();
	MemoryContext oldcxt;
	uint8		backend_type_code;
	uint8		database_oid_code;
	uint8		user_oid_code;
	uint8		ctrl;

	if (MyProcPort)
	{
		database_name = MyProcPort->database_name;
		authn_user = MyProcPort->user_name;
		remote_host = MyProcPort->remote_host;
		remote_port = MyProcPort->remote_port;
		if (MyProcPort->application_name &&
			MyProcPort->application_name[0] != '\0')
				appname = MyProcPort->application_name;
	}

	if (aez_context_cache_initialized &&
		aez_context_backend_type == MyBackendType &&
		aez_context_database_id == MyDatabaseId &&
		aez_context_user_id == GetUserId() &&
		aez_context_appname != NULL &&
		appname != NULL &&
		strcmp(aez_context_appname, appname) == 0)
	{
		aez_put_u8(buf, AEZ_CONTEXT_CTRL_REF);
		return;
	}

	if (database_name == NULL && OidIsValid(MyDatabaseId))
		database_name = get_database_name(MyDatabaseId);
	if (authn_user == NULL && OidIsValid(GetUserId()))
		authn_user = GetUserNameFromId(GetUserId(), true);
	if (OidIsValid(GetUserId()))
		effective_user = GetUserNameFromId(GetUserId(), true);

	backend_type_code = aez_uint_size_code((uint32) MyBackendType);
	database_oid_code = aez_uint_size_code(MyDatabaseId);
	user_oid_code = aez_uint_size_code(GetUserId());
	ctrl = backend_type_code |
		(database_oid_code << 2) |
		(user_oid_code << 4);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	if (!aez_context_cache_initialized)
	{
		initStringInfo(&aez_context_cache);
		aez_context_cache_initialized = true;
	}
	else
		resetStringInfo(&aez_context_cache);
	if (aez_context_appname)
		pfree(aez_context_appname);
	aez_context_appname = pstrdup(appname ? appname : "");
	MemoryContextSwitchTo(oldcxt);

	aez_put_u8(&aez_context_cache, ctrl);
	aez_put_uint_sized(&aez_context_cache, (uint32) MyBackendType,
					   backend_type_code);
	aez_put_string(&aez_context_cache, backend_type, -1);
	aez_put_uint_sized(&aez_context_cache, MyDatabaseId, database_oid_code);
	aez_put_string(&aez_context_cache, database_name, -1);
	aez_put_uint_sized(&aez_context_cache, GetUserId(), user_oid_code);
	aez_put_string(&aez_context_cache, authn_user, -1);
	aez_put_string(&aez_context_cache, effective_user, -1);
	aez_put_string(&aez_context_cache, appname, -1);
	aez_put_string(&aez_context_cache, remote_host, -1);
	aez_put_string(&aez_context_cache, remote_port, -1);

	aez_context_backend_type = MyBackendType;
	aez_context_database_id = MyDatabaseId;
	aez_context_user_id = GetUserId();

	appendBinaryStringInfo(buf, aez_context_cache.data,
						   aez_context_cache.len);
}

static bool
aez_context_can_reference(void)
{
	const char *appname = application_name;

	if (MyProcPort &&
		MyProcPort->application_name &&
		MyProcPort->application_name[0] != '\0')
		appname = MyProcPort->application_name;

	return aez_context_cache_initialized &&
		aez_context_backend_type == MyBackendType &&
		aez_context_database_id == MyDatabaseId &&
		aez_context_user_id == GetUserId() &&
		aez_context_appname != NULL &&
		appname != NULL &&
		strcmp(aez_context_appname, appname) == 0;
}

static bool
aez_query_may_have_details(QueryDesc *queryDesc, uint32 payload_flags,
						   BufferUsage *planning_usage)
{
	EState	   *estate = queryDesc->estate;

	if (estate == NULL)
		return false;

	if ((payload_flags & AEZ_QUERY_FLAG_BUFFERS) != 0 &&
		planning_usage != NULL &&
		aez_buffer_usage_any(planning_usage))
		return true;

	if (estate->es_jit_flags & PGJIT_PERFORM)
		return true;

	if ((payload_flags & AEZ_QUERY_FLAG_ANALYZE) == 0)
		return false;

	return estate->es_opened_result_relations != NIL ||
		estate->es_tuple_routing_result_relations != NIL ||
		estate->es_trig_target_relations != NIL;
}

static bool
aez_fast_param_value(ParamExternData *param, char *value, Size valuesize)
{
	int			written;

	switch (param->ptype)
	{
		case BOOLOID:
			value[0] = DatumGetBool(param->value) ? 't' : 'f';
			value[1] = '\0';
			return true;
		case INT2OID:
			written = snprintf(value, valuesize, "%d",
							   (int) DatumGetInt16(param->value));
			break;
		case INT4OID:
			written = snprintf(value, valuesize, "%d",
							   DatumGetInt32(param->value));
			break;
		case INT8OID:
			written = snprintf(value, valuesize, INT64_FORMAT,
					 DatumGetInt64(param->value));
			break;
		case OIDOID:
			written = snprintf(value, valuesize, "%u",
							   DatumGetObjectId(param->value));
			break;
		default:
			return false;
	}

	return written >= 0 && (Size) written < valuesize;
}

static bool
aez_append_fixed(char *dst, Size dstlen, Size *pos, const char *src,
				 Size srclen)
{
	if (*pos > dstlen || srclen >= dstlen - *pos)
		return false;
	memcpy(dst + *pos, src, srclen);
	*pos += srclen;
	dst[*pos] = '\0';
	return true;
}

static bool
aez_append_ascii_quoted_fixed(char *dst, Size dstlen, Size *pos,
							  const char *str, int maxlen)
{
	Size		len = strlen(str);
	Size		copylen = len;
	bool		ellipsis = false;

	if (maxlen >= 0 && (Size) maxlen < len)
	{
		copylen = (Size) maxlen;
		ellipsis = true;
	}

	return aez_append_fixed(dst, dstlen, pos, "'", 1) &&
		aez_append_fixed(dst, dstlen, pos, str, copylen) &&
		(!ellipsis || aez_append_fixed(dst, dstlen, pos, "...'", 4)) &&
		(ellipsis || aez_append_fixed(dst, dstlen, pos, "'", 1));
}

static bool
aez_try_build_fast_param_log_string(ParamListInfo params, char *dst,
									Size dstlen)
{
	Size		pos = 0;

	if (dstlen == 0)
		return false;
	dst[0] = '\0';

	if (params->paramFetch != NULL || IsAbortedTransactionBlockState())
		return false;

	for (int paramno = 0; paramno < params->numParams; paramno++)
	{
		ParamExternData *param = &params->params[paramno];
		char		value[64];

		if (param->isnull || !OidIsValid(param->ptype))
			continue;
		if (!aez_fast_param_value(param, value, sizeof(value)))
			return false;
	}

	for (int paramno = 0; paramno < params->numParams; paramno++)
	{
		ParamExternData *param = &params->params[paramno];
		char		prefix[32];
		char		value[64];
		int			written;

		written = snprintf(prefix, sizeof(prefix), "%s$%d = ",
						   paramno > 0 ? ", " : "", paramno + 1);
		if (written < 0 || (Size) written >= sizeof(prefix) ||
			!aez_append_fixed(dst, dstlen, &pos, prefix, (Size) written))
			return false;

		if (param->isnull || !OidIsValid(param->ptype))
		{
			if (!aez_append_fixed(dst, dstlen, &pos, "NULL", 4))
				return false;
		}
		else
		{
			if (!aez_fast_param_value(param, value, sizeof(value)) ||
				!aez_append_ascii_quoted_fixed(dst, dstlen, &pos, value,
											   auto_explain_z_log_parameter_max_length))
				return false;
		}
	}

	return true;
}

static char *
aez_build_param_log_string(ParamListInfo params)
{
	StringInfoData buf;

	if (params->paramFetch != NULL || IsAbortedTransactionBlockState())
		return NULL;

	for (int paramno = 0; paramno < params->numParams; paramno++)
	{
		ParamExternData *param = &params->params[paramno];
		char		value[64];

		if (param->isnull || !OidIsValid(param->ptype))
			continue;
		if (!aez_fast_param_value(param, value, sizeof(value)))
			return BuildParamLogString(params, NULL,
									   auto_explain_z_log_parameter_max_length);
	}

	initStringInfo(&buf);
	for (int paramno = 0; paramno < params->numParams; paramno++)
	{
		ParamExternData *param = &params->params[paramno];

		appendStringInfo(&buf, "%s$%d = ", paramno > 0 ? ", " : "",
						 paramno + 1);
		if (param->isnull || !OidIsValid(param->ptype))
			appendStringInfoString(&buf, "NULL");
		else
			(void) aez_append_fast_param_value(&buf, param,
											   auto_explain_z_log_parameter_max_length);
	}

	return buf.data;
}

static bool
aez_append_fast_param_value(StringInfo buf, ParamExternData *param, int maxlen)
{
	char		value[64];

	if (!aez_fast_param_value(param, value, sizeof(value)))
		return false;

	aez_append_ascii_quoted(buf, value, maxlen);
	return true;
}

static void
aez_append_ascii_quoted(StringInfo buf, const char *str, int maxlen)
{
	int			len = strlen(str);
	int			copylen = len;
	bool		ellipsis = false;

	if (maxlen >= 0 && maxlen < len)
	{
		copylen = maxlen;
		ellipsis = true;
	}

	appendStringInfoCharMacro(buf, '\'');
	appendBinaryStringInfo(buf, str, copylen);
	if (ellipsis)
		appendStringInfoString(buf, "...'");
	else
		appendStringInfoCharMacro(buf, '\'');
}

static bool
aez_serialize_query_details(StringInfo buf, QueryDesc *queryDesc,
							uint32 payload_flags,
							BufferUsage *planning_usage)
{
	StringInfoData details;
	int			detail_count = 0;

	initStringInfo(&details);
	if ((payload_flags & AEZ_QUERY_FLAG_BUFFERS) != 0 &&
		planning_usage != NULL &&
		aez_buffer_usage_any(planning_usage))
		aez_serialize_planning_buffers(&details, &detail_count,
									   planning_usage);
	if (payload_flags & AEZ_QUERY_FLAG_ANALYZE)
		aez_serialize_triggers(&details, &detail_count, queryDesc,
							   (payload_flags & AEZ_QUERY_FLAG_TIMING) != 0);
	aez_serialize_jit_summary(&details, &detail_count, queryDesc,
							  payload_flags);

	if (detail_count == 0)
		return false;

	aez_put_u16(buf, (uint16) detail_count);
	appendBinaryStringInfo(buf, details.data, details.len);
	return true;
}

static void
aez_serialize_planning_buffers(StringInfo details, int *detail_count,
							   BufferUsage *usage)
{
	const char *values[16];

	values[0] = psprintf(INT64_FORMAT, usage->shared_blks_hit);
	values[1] = psprintf(INT64_FORMAT, usage->shared_blks_read);
	values[2] = psprintf(INT64_FORMAT, usage->shared_blks_dirtied);
	values[3] = psprintf(INT64_FORMAT, usage->shared_blks_written);
	values[4] = psprintf(INT64_FORMAT, usage->local_blks_hit);
	values[5] = psprintf(INT64_FORMAT, usage->local_blks_read);
	values[6] = psprintf(INT64_FORMAT, usage->local_blks_dirtied);
	values[7] = psprintf(INT64_FORMAT, usage->local_blks_written);
	values[8] = psprintf(INT64_FORMAT, usage->temp_blks_read);
	values[9] = psprintf(INT64_FORMAT, usage->temp_blks_written);
	values[10] = psprintf(INT64_FORMAT,
						   INSTR_TIME_GET_MICROSEC(usage->shared_blk_read_time));
	values[11] = psprintf(INT64_FORMAT,
						   INSTR_TIME_GET_MICROSEC(usage->shared_blk_write_time));
	values[12] = psprintf(INT64_FORMAT,
						   INSTR_TIME_GET_MICROSEC(usage->local_blk_read_time));
	values[13] = psprintf(INT64_FORMAT,
						   INSTR_TIME_GET_MICROSEC(usage->local_blk_write_time));
	values[14] = psprintf(INT64_FORMAT,
						   INSTR_TIME_GET_MICROSEC(usage->temp_blk_read_time));
	values[15] = psprintf(INT64_FORMAT,
						   INSTR_TIME_GET_MICROSEC(usage->temp_blk_write_time));

	aez_detail_cstring_array(details, detail_count,
							 AEZ_DETAIL_PLANNING_BUFFERS, 16, values);
}

static void
aez_serialize_triggers(StringInfo details, int *detail_count,
					   QueryDesc *queryDesc, bool timing)
{
	List	   *resultrels;
	List	   *routerels;
	List	   *targrels;
	bool		show_relname;

	if (!queryDesc->estate)
		return;

	resultrels = queryDesc->estate->es_opened_result_relations;
	routerels = queryDesc->estate->es_tuple_routing_result_relations;
	targrels = queryDesc->estate->es_trig_target_relations;

	show_relname = (list_length(resultrels) > 1 ||
					routerels != NIL || targrels != NIL);

	aez_serialize_trigger_list(details, detail_count, resultrels,
							   show_relname, timing);
	aez_serialize_trigger_list(details, detail_count, routerels,
							   show_relname, timing);
	aez_serialize_trigger_list(details, detail_count, targrels,
							   show_relname, timing);
}

static void
aez_serialize_trigger_list(StringInfo details, int *detail_count,
						   List *result_rels, bool show_relname, bool timing)
{
	ListCell   *lc;

	foreach(lc, result_rels)
	{
		ResultRelInfo *rInfo = (ResultRelInfo *) lfirst(lc);

		if (!rInfo->ri_TrigDesc || !rInfo->ri_TrigInstrument)
			continue;

		for (int nt = 0; nt < rInfo->ri_TrigDesc->numtriggers; nt++)
		{
			Trigger    *trig = rInfo->ri_TrigDesc->triggers + nt;
			Instrumentation *instr = rInfo->ri_TrigInstrument + nt;
			const char *relname = NULL;
			char	   *conname = NULL;
			char	   *time_us;
			char	   *calls;
			const char *values[5];

			InstrEndLoop(instr);
			if (instr->ntuples == 0)
				continue;

			if (show_relname && rInfo->ri_RelationDesc)
				relname = RelationGetRelationName(rInfo->ri_RelationDesc);
			if (OidIsValid(trig->tgconstraint))
				conname = get_constraint_name(trig->tgconstraint);

			time_us = psprintf(INT64_FORMAT,
							   timing ? AEZ_SECONDS_TO_MICROSECONDS(instr->total) : 0);
			calls = psprintf("%.0f", instr->ntuples);
			values[0] = trig->tgname;
			values[1] = conname;
			values[2] = relname;
			values[3] = time_us;
			values[4] = calls;
			aez_detail_cstring_array(details, detail_count,
									 AEZ_DETAIL_TRIGGER, 5, values);
			if (conname)
				pfree(conname);
		}
	}
}

static void
aez_serialize_jit_summary(StringInfo details, int *detail_count,
						  QueryDesc *queryDesc, uint32 payload_flags)
{
	EState	   *estate = queryDesc->estate;
	JitInstrumentation ji = {0};
	instr_time	total_time;
	const char *values[13];
	char	   *functions;
	char	   *flags;
	char	   *generation_us;
	char	   *deform_us;
	char	   *inlining_us;
	char	   *optimization_us;
	char	   *emission_us;
	char	   *total_us;

	if (!estate || !(estate->es_jit_flags & PGJIT_PERFORM))
		return;

	if (estate->es_jit)
		InstrJitAgg(&ji, &estate->es_jit->instr);
	if (estate->es_jit_worker_instr)
		InstrJitAgg(&ji, estate->es_jit_worker_instr);
	if (ji.created_functions == 0)
		return;

	INSTR_TIME_SET_ZERO(total_time);
	INSTR_TIME_ADD(total_time, ji.generation_counter);
	INSTR_TIME_ADD(total_time, ji.inlining_counter);
	INSTR_TIME_ADD(total_time, ji.optimization_counter);
	INSTR_TIME_ADD(total_time, ji.emission_counter);

	functions = psprintf(UINT64_FORMAT, (uint64) ji.created_functions);
	flags = psprintf("%d", estate->es_jit_flags);
	generation_us = psprintf(INT64_FORMAT,
							 (int64) INSTR_TIME_GET_MICROSEC(ji.generation_counter));
	deform_us = psprintf(INT64_FORMAT,
						 (int64) INSTR_TIME_GET_MICROSEC(ji.deform_counter));
	inlining_us = psprintf(INT64_FORMAT,
						   (int64) INSTR_TIME_GET_MICROSEC(ji.inlining_counter));
	optimization_us = psprintf(INT64_FORMAT,
							   (int64) INSTR_TIME_GET_MICROSEC(ji.optimization_counter));
	emission_us = psprintf(INT64_FORMAT,
						   (int64) INSTR_TIME_GET_MICROSEC(ji.emission_counter));
	total_us = psprintf(INT64_FORMAT,
						(int64) INSTR_TIME_GET_MICROSEC(total_time));

	values[0] = functions;
	values[1] = flags;
	values[2] = (estate->es_jit_flags & PGJIT_INLINE) ? "true" : "false";
	values[3] = (estate->es_jit_flags & PGJIT_OPT3) ? "true" : "false";
	values[4] = (estate->es_jit_flags & PGJIT_EXPR) ? "true" : "false";
	values[5] = (estate->es_jit_flags & PGJIT_DEFORM) ? "true" : "false";
	values[6] = ((payload_flags & AEZ_QUERY_FLAG_ANALYZE) &&
				 (payload_flags & AEZ_QUERY_FLAG_TIMING)) ? "true" : "false";
	values[7] = generation_us;
	values[8] = deform_us;
	values[9] = inlining_us;
	values[10] = optimization_us;
	values[11] = emission_us;
	values[12] = total_us;
	aez_detail_cstring_array(details, detail_count,
							 AEZ_DETAIL_JIT_SUMMARY, 13, values);
}

static uint64
aez_plan_shape_hash(AezSerializeState *state, PlanState *planstate,
					AezPlanRelationship relationship)
{
	uint64		hash = UINT64CONST(0x9e3779b97f4a7c15);

	hash = aez_hash_u32(hash, AEZ_FORMAT_VERSION);
	hash = aez_hash_u32(hash, (uint32) auto_explain_z_profile);
	hash = aez_hash_bool(hash, state->verbose);
	return aez_hash_plan_shape_node(state, hash, planstate, relationship);
}

static uint64
aez_hash_plan_shape_node(AezSerializeState *state, uint64 hash,
						 PlanState *planstate,
						 AezPlanRelationship relationship)
{
	Plan	   *plan = planstate->plan;
	int			child_count;

	hash = aez_hash_u32(hash, (uint32) relationship);
	hash = aez_hash_u32(hash, (uint32) nodeTag(plan));
	hash = aez_hash_bool(hash, plan->parallel_aware);
	hash = aez_hash_bool(hash, plan->async_capable);
	hash = aez_hash_plan_static_fields(state, hash, plan);

	child_count = aez_count_plan_children(planstate);
	hash = aez_hash_u32(hash, (uint32) child_count);
	hash = aez_hash_plan_shape_children(state, hash, planstate);

	return hash;
}

static uint64
aez_hash_plan_shape_children(AezSerializeState *state, uint64 hash,
							 PlanState *planstate)
{
	Plan	   *plan = planstate->plan;

	hash = aez_hash_plan_shape_subplans(state, hash, planstate->initPlan,
										AEZ_PLAN_REL_INITPLAN);

	if (outerPlanState(planstate))
		hash = aez_hash_plan_shape_node(state, hash,
										outerPlanState(planstate),
										AEZ_PLAN_REL_OUTER);
	if (innerPlanState(planstate))
		hash = aez_hash_plan_shape_node(state, hash,
										innerPlanState(planstate),
										AEZ_PLAN_REL_INNER);

	switch (nodeTag(plan))
	{
		case T_Append:
			hash = aez_hash_plan_shape_members(state, hash,
											   ((AppendState *) planstate)->appendplans,
											   ((AppendState *) planstate)->as_nplans,
											   AEZ_PLAN_REL_MEMBER);
			break;
		case T_MergeAppend:
			hash = aez_hash_plan_shape_members(state, hash,
											   ((MergeAppendState *) planstate)->mergeplans,
											   ((MergeAppendState *) planstate)->ms_nplans,
											   AEZ_PLAN_REL_MEMBER);
			break;
		case T_BitmapAnd:
			hash = aez_hash_plan_shape_members(state, hash,
											   ((BitmapAndState *) planstate)->bitmapplans,
											   ((BitmapAndState *) planstate)->nplans,
											   AEZ_PLAN_REL_MEMBER);
			break;
		case T_BitmapOr:
			hash = aez_hash_plan_shape_members(state, hash,
											   ((BitmapOrState *) planstate)->bitmapplans,
											   ((BitmapOrState *) planstate)->nplans,
											   AEZ_PLAN_REL_MEMBER);
			break;
		case T_SubqueryScan:
			hash = aez_hash_plan_shape_node(state, hash,
											((SubqueryScanState *) planstate)->subplan,
											AEZ_PLAN_REL_SUBQUERY);
			break;
		case T_CustomScan:
			{
				ListCell   *lc;

				foreach(lc, ((CustomScanState *) planstate)->custom_ps)
					hash = aez_hash_plan_shape_node(state, hash,
													(PlanState *) lfirst(lc),
													AEZ_PLAN_REL_CUSTOM);
			}
			break;
		default:
			break;
	}

	hash = aez_hash_plan_shape_subplans(state, hash, planstate->subPlan,
										AEZ_PLAN_REL_SUBPLAN);

	return hash;
}

static uint64
aez_hash_plan_shape_members(AezSerializeState *state, uint64 hash,
							PlanState **planstates, int nplans,
							AezPlanRelationship relationship)
{
	for (int i = 0; i < nplans; i++)
		hash = aez_hash_plan_shape_node(state, hash, planstates[i],
										relationship);
	return hash;
}

static uint64
aez_hash_plan_shape_subplans(AezSerializeState *state, uint64 hash,
							 List *plans,
							 AezPlanRelationship relationship)
{
	ListCell   *lc;

	foreach(lc, plans)
	{
		SubPlanState *sps = lfirst_node(SubPlanState, lc);

		hash = aez_hash_plan_shape_node(state, hash, sps->planstate,
										relationship);
	}

	return hash;
}

static uint64
aez_hash_plan_static_fields(AezSerializeState *state, uint64 hash, Plan *plan)
{
	hash = aez_hash_u32(hash, (uint32) plan->disabled_nodes);

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapIndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
		case T_ForeignScan:
		case T_CustomScan:
			{
				Index		scanrelid = ((Scan *) plan)->scanrelid;

				hash = aez_hash_u32(hash, (uint32) scanrelid);
				if (scanrelid > 0 && scanrelid <= list_length(state->rtable))
				{
					RangeTblEntry *rte = rt_fetch(scanrelid, state->rtable);

					hash = aez_hash_u32(hash, (uint32) rte->rtekind);
					if (rte->rtekind == RTE_RELATION)
						hash = aez_hash_u32(hash, rte->relid);
				}
			}
			break;
		default:
			break;
	}

	if (auto_explain_z_profile == AEZ_PROFILE_FULL)
	{
		hash = aez_hash_node_string(hash, plan->qual);
		if (state->verbose)
			hash = aez_hash_node_string(hash, plan->targetlist);
	}

	switch (nodeTag(plan))
	{
		case T_ModifyTable:
			hash = aez_hash_u32(hash,
								(uint32) ((ModifyTable *) plan)->operation);
			hash = aez_hash_node_string(hash,
										((ModifyTable *) plan)->resultRelations);
			hash = aez_hash_node_string(hash,
										((ModifyTable *) plan)->returningLists);
			hash = aez_hash_node_string(hash,
										((ModifyTable *) plan)->withCheckOptionLists);
			hash = aez_hash_node_string(hash,
										((ModifyTable *) plan)->mergeActionLists);
			break;
		case T_ForeignScan:
			hash = aez_hash_u32(hash,
								(uint32) ((ForeignScan *) plan)->operation);
			hash = aez_hash_u32(hash,
								(uint32) ((ForeignScan *) plan)->fs_server);
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
			{
				hash = aez_hash_node_string(hash,
											((ForeignScan *) plan)->fdw_exprs);
				hash = aez_hash_node_string(hash,
											((ForeignScan *) plan)->fdw_recheck_quals);
			}
			break;
		case T_CustomScan:
			hash = aez_hash_node_string(hash,
										((CustomScan *) plan)->custom_relids);
			if (((CustomScan *) plan)->methods &&
				((CustomScan *) plan)->methods->CustomName)
				hash = aez_hash_bytes_value(hash,
											((CustomScan *) plan)->methods->CustomName,
											strlen(((CustomScan *) plan)->methods->CustomName));
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
				hash = aez_hash_node_string(hash,
											((CustomScan *) plan)->custom_exprs);
			break;
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			hash = aez_hash_u32(hash, (uint32) ((Join *) plan)->jointype);
			hash = aez_hash_bool(hash, ((Join *) plan)->inner_unique);
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
				hash = aez_hash_node_string(hash, ((Join *) plan)->joinqual);
			break;
		default:
			break;
	}

	switch (nodeTag(plan))
	{
		case T_IndexScan:
			hash = aez_hash_u32(hash, ((IndexScan *) plan)->indexid);
			hash = aez_hash_u32(hash,
								(uint32) ((IndexScan *) plan)->indexorderdir);
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
			{
				hash = aez_hash_node_string(hash,
											((IndexScan *) plan)->indexqualorig);
				hash = aez_hash_node_string(hash,
											((IndexScan *) plan)->indexorderbyorig);
			}
			break;
		case T_IndexOnlyScan:
			hash = aez_hash_u32(hash, ((IndexOnlyScan *) plan)->indexid);
			hash = aez_hash_u32(hash,
								(uint32) ((IndexOnlyScan *) plan)->indexorderdir);
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
			{
				hash = aez_hash_node_string(hash,
											((IndexOnlyScan *) plan)->indexqual);
				hash = aez_hash_node_string(hash,
											((IndexOnlyScan *) plan)->recheckqual);
				hash = aez_hash_node_string(hash,
											((IndexOnlyScan *) plan)->indexorderby);
			}
			break;
		case T_BitmapIndexScan:
			hash = aez_hash_u32(hash, ((BitmapIndexScan *) plan)->indexid);
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
				hash = aez_hash_node_string(hash,
											((BitmapIndexScan *) plan)->indexqualorig);
			break;
		case T_BitmapHeapScan:
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
				hash = aez_hash_node_string(hash,
											((BitmapHeapScan *) plan)->bitmapqualorig);
			break;
		case T_TidScan:
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
				hash = aez_hash_node_string(hash,
											((TidScan *) plan)->tidquals);
			break;
		case T_TidRangeScan:
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
				hash = aez_hash_node_string(hash,
											((TidRangeScan *) plan)->tidrangequals);
			break;
		case T_FunctionScan:
			if (auto_explain_z_profile == AEZ_PROFILE_FULL && state->verbose)
				hash = aez_hash_node_string(hash,
											((FunctionScan *) plan)->functions);
			break;
		case T_TableFuncScan:
			if (auto_explain_z_profile == AEZ_PROFILE_FULL && state->verbose)
				hash = aez_hash_node_string(hash,
											((TableFuncScan *) plan)->tablefunc);
			break;
		case T_MergeJoin:
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
				hash = aez_hash_node_string(hash,
											((MergeJoin *) plan)->mergeclauses);
			break;
		case T_HashJoin:
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
				hash = aez_hash_node_string(hash,
											((HashJoin *) plan)->hashclauses);
			break;
		case T_Agg:
			hash = aez_hash_u32(hash,
								(uint32) ((Agg *) plan)->aggstrategy);
			hash = aez_hash_u32(hash,
								(uint32) ((Agg *) plan)->aggsplit);
			hash = aez_hash_u32(hash, (uint32) ((Agg *) plan)->numCols);
			hash = aez_hash_bytes_value(hash, ((Agg *) plan)->grpColIdx,
										sizeof(AttrNumber) *
										((Agg *) plan)->numCols);
			hash = aez_hash_node_string(hash,
										((Agg *) plan)->groupingSets);
			hash = aez_hash_node_string(hash, ((Agg *) plan)->chain);
			break;
		case T_WindowAgg:
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
			{
				hash = aez_hash_node_string(hash,
											((WindowAgg *) plan)->runConditionOrig);
				hash = aez_hash_u32(hash,
									(uint32) ((WindowAgg *) plan)->partNumCols);
				hash = aez_hash_bytes_value(hash,
											((WindowAgg *) plan)->partColIdx,
											sizeof(AttrNumber) *
											((WindowAgg *) plan)->partNumCols);
				hash = aez_hash_u32(hash,
									(uint32) ((WindowAgg *) plan)->ordNumCols);
				hash = aez_hash_bytes_value(hash,
											((WindowAgg *) plan)->ordColIdx,
											sizeof(AttrNumber) *
											((WindowAgg *) plan)->ordNumCols);
			}
			break;
		case T_Group:
			hash = aez_hash_u32(hash, (uint32) ((Group *) plan)->numCols);
			hash = aez_hash_bytes_value(hash, ((Group *) plan)->grpColIdx,
										sizeof(AttrNumber) *
										((Group *) plan)->numCols);
			break;
		case T_Sort:
			hash = aez_hash_u32(hash, (uint32) ((Sort *) plan)->numCols);
			hash = aez_hash_bytes_value(hash, ((Sort *) plan)->sortColIdx,
										sizeof(AttrNumber) *
										((Sort *) plan)->numCols);
			hash = aez_hash_bytes_value(hash, ((Sort *) plan)->sortOperators,
										sizeof(Oid) * ((Sort *) plan)->numCols);
			hash = aez_hash_bytes_value(hash, ((Sort *) plan)->collations,
										sizeof(Oid) * ((Sort *) plan)->numCols);
			hash = aez_hash_bytes_value(hash, ((Sort *) plan)->nullsFirst,
										sizeof(bool) * ((Sort *) plan)->numCols);
			break;
		case T_IncrementalSort:
			hash = aez_hash_u32(hash,
								(uint32) ((IncrementalSort *) plan)->sort.numCols);
			hash = aez_hash_u32(hash,
								(uint32) ((IncrementalSort *) plan)->nPresortedCols);
			hash = aez_hash_bytes_value(hash,
										((IncrementalSort *) plan)->sort.sortColIdx,
										sizeof(AttrNumber) *
										((IncrementalSort *) plan)->sort.numCols);
			hash = aez_hash_bytes_value(hash,
										((IncrementalSort *) plan)->sort.sortOperators,
										sizeof(Oid) *
										((IncrementalSort *) plan)->sort.numCols);
			hash = aez_hash_bytes_value(hash,
										((IncrementalSort *) plan)->sort.collations,
										sizeof(Oid) *
										((IncrementalSort *) plan)->sort.numCols);
			hash = aez_hash_bytes_value(hash,
										((IncrementalSort *) plan)->sort.nullsFirst,
										sizeof(bool) *
										((IncrementalSort *) plan)->sort.numCols);
			break;
		case T_MergeAppend:
			hash = aez_hash_u32(hash, (uint32) ((MergeAppend *) plan)->numCols);
			hash = aez_hash_bytes_value(hash,
										((MergeAppend *) plan)->sortColIdx,
										sizeof(AttrNumber) *
										((MergeAppend *) plan)->numCols);
			hash = aez_hash_bytes_value(hash,
										((MergeAppend *) plan)->sortOperators,
										sizeof(Oid) *
										((MergeAppend *) plan)->numCols);
			hash = aez_hash_bytes_value(hash,
										((MergeAppend *) plan)->collations,
										sizeof(Oid) *
										((MergeAppend *) plan)->numCols);
			hash = aez_hash_bytes_value(hash,
										((MergeAppend *) plan)->nullsFirst,
										sizeof(bool) *
										((MergeAppend *) plan)->numCols);
			break;
		case T_Gather:
			hash = aez_hash_u32(hash, (uint32) ((Gather *) plan)->num_workers);
			hash = aez_hash_bool(hash, ((Gather *) plan)->single_copy);
			break;
		case T_GatherMerge:
			hash = aez_hash_u32(hash,
								(uint32) ((GatherMerge *) plan)->num_workers);
			hash = aez_hash_u32(hash, (uint32) ((GatherMerge *) plan)->numCols);
			hash = aez_hash_bytes_value(hash,
										((GatherMerge *) plan)->sortColIdx,
										sizeof(AttrNumber) *
										((GatherMerge *) plan)->numCols);
			hash = aez_hash_bytes_value(hash,
										((GatherMerge *) plan)->sortOperators,
										sizeof(Oid) *
										((GatherMerge *) plan)->numCols);
			hash = aez_hash_bytes_value(hash,
										((GatherMerge *) plan)->collations,
										sizeof(Oid) *
										((GatherMerge *) plan)->numCols);
			hash = aez_hash_bytes_value(hash,
										((GatherMerge *) plan)->nullsFirst,
										sizeof(bool) *
										((GatherMerge *) plan)->numCols);
			break;
		case T_Result:
			if (auto_explain_z_profile == AEZ_PROFILE_FULL)
				hash = aez_hash_node_string(hash,
											((Result *) plan)->resconstantqual);
			break;
		default:
			break;
	}

	return hash;
}

static uint64
aez_hash_node_string(uint64 hash, const void *node)
{
	char	   *node_string;

	if (node == NULL)
		return aez_hash_u32(hash, 0);

	node_string = nodeToString(node);
	hash = aez_hash_u32(hash, (uint32) strlen(node_string));
	hash = aez_hash_bytes_value(hash, node_string, strlen(node_string));
	pfree(node_string);
	return hash;
}

static uint64
aez_hash_bytes_value(uint64 hash, const void *data, Size len)
{
	const unsigned char *ptr = (const unsigned char *) data;

	hash = aez_hash_u64(hash, (uint64) len);
	if (data == NULL || len == 0)
		return hash;

	while (len > 0)
	{
		int			chunk = (len > PG_INT32_MAX) ? PG_INT32_MAX : (int) len;

		hash = hash_bytes_extended(ptr, chunk, hash);
		ptr += chunk;
		len -= chunk;
	}

	return hash;
}

static uint64
aez_hash_u32(uint64 hash, uint32 value)
{
	return hash_combine64(hash, murmurhash64((uint64) value));
}

static uint64
aez_hash_u64(uint64 hash, uint64 value)
{
	return hash_combine64(hash, murmurhash64(value));
}

static uint64
aez_hash_bool(uint64 hash, bool value)
{
	return aez_hash_u32(hash, value ? 1 : 0);
}

static int64
aez_explain_time_microseconds(double seconds)
{
	char		buf[64];
	double		milliseconds;

	snprintf(buf, sizeof(buf), "%.3f", 1000.0 * seconds);
	milliseconds = strtod(buf, NULL);
	return (int64) (milliseconds * 1000.0 + 0.5);
}

static void
aez_reset_templates(void)
{
	if (aez_template_hash)
	{
		hash_destroy(aez_template_hash);
		aez_template_hash = NULL;
	}
	if (aez_query_template_hash)
	{
		hash_destroy(aez_query_template_hash);
		aez_query_template_hash = NULL;
	}
	aez_template_count = 0;
	aez_next_template_id = 1;
}

static void
aez_invalidate_context_cache(void)
{
	aez_context_backend_type = B_INVALID;
	aez_context_database_id = InvalidOid;
	aez_context_user_id = InvalidOid;
	if (aez_context_appname)
	{
		pfree(aez_context_appname);
		aez_context_appname = NULL;
	}
}

static AezTemplateEntry *
aez_find_template(AezTemplateKey *key)
{
	if (aez_template_hash == NULL)
		return NULL;

	return (AezTemplateEntry *) hash_search(aez_template_hash, key,
											HASH_FIND, NULL);
}

static AezTemplateEntry *
aez_add_template(AezTemplateKey *key, uint32 plan_bytes)
{
	HASHCTL		ctl;
	AezTemplateEntry *entry;
	bool		found;

	if (aez_template_count >= (uint32) auto_explain_z_max_templates)
		return NULL;

	if (aez_template_hash == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(AezTemplateKey);
		ctl.entrysize = sizeof(AezTemplateEntry);
		ctl.hcxt = TopMemoryContext;
		aez_template_hash = hash_create("auto_explain_z plan templates",
										auto_explain_z_max_templates,
										&ctl,
										HASH_ELEM | HASH_BLOBS |
										HASH_CONTEXT);
	}

	entry = (AezTemplateEntry *) hash_search(aez_template_hash, key,
											HASH_ENTER, &found);
	if (found)
		return entry;

	entry->template_id = aez_next_template_id++;
	entry->plan_bytes = plan_bytes;
	aez_template_count++;
	return entry;
}

static AezQueryTemplateEntry *
aez_find_query_template(AezQueryTemplateKey *key)
{
	if (aez_query_template_hash == NULL)
		return NULL;

	return (AezQueryTemplateEntry *) hash_search(aez_query_template_hash, key,
												 HASH_FIND, NULL);
}

static void
aez_remember_query_template(AezQueryTemplateKey *key,
							AezTemplateEntry *template_entry)
{
	HASHCTL		ctl;
	AezQueryTemplateEntry *entry;
	bool		found;

	if (key->query_id == 0 || template_entry == NULL)
		return;

	if (aez_query_template_hash == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(AezQueryTemplateKey);
		ctl.entrysize = sizeof(AezQueryTemplateEntry);
		ctl.hcxt = TopMemoryContext;
		aez_query_template_hash =
			hash_create("auto_explain_z query template fast path",
						auto_explain_z_max_templates,
						&ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	entry = (AezQueryTemplateEntry *) hash_search(aez_query_template_hash, key,
												 HASH_ENTER, &found);
	entry->shape_hash = template_entry->key.shape_hash;
	entry->template_id = template_entry->template_id;
	entry->plan_bytes = template_entry->plan_bytes;
	entry->ref_count = 0;
}

static bool
aez_write_record(StringInfo record)
{
	uint64		pending_limit;

	if (aez_fd < 0 && !aez_open_file(0))
		return false;

	pending_limit = (uint64) auto_explain_z_pending_buffer_size * 1024;
	if (pending_limit == 0)
	{
		if (!aez_flush_pending() ||
			!aez_write_file_data(record->data, record->len))
		{
			aez_close_file();
			aez_writer_warning("could not write auto_explain_z binary log record");
			return false;
		}

		aez_file_uncompressed_bytes += record->len;
		return true;
	}

	aez_init_pending_buffer();
	if ((uint64) record->len >= pending_limit)
	{
		if (!aez_flush_pending() ||
			!aez_write_file_data(record->data, record->len))
		{
			aez_close_file();
			aez_writer_warning("could not write auto_explain_z binary log record");
			return false;
		}
	}
	else
	{
		appendBinaryStringInfo(&aez_pending, record->data, record->len);
		if ((uint64) aez_pending.len >= pending_limit &&
			!aez_flush_pending())
		{
			aez_close_file();
			aez_writer_warning("could not write auto_explain_z binary log record");
			return false;
		}
	}

	aez_file_uncompressed_bytes += record->len;
	return true;
}

static bool
aez_file_preflight_needed(void)
{
	uint64		max_bytes;

	if (aez_fd < 0)
		return true;
	if (aez_current_directory_guc != auto_explain_z_directory ||
		aez_current_prefix_guc != auto_explain_z_file_prefix)
		return true;
	if (aez_file_compression != aez_select_compression())
		return true;

	max_bytes = (uint64) auto_explain_z_max_file_size * 1024;
	return max_bytes > 0 && aez_file_uncompressed_bytes >= max_bytes;
}

static bool
aez_open_file(uint64 needed)
{
	char	   *path;
	StringInfoData header;
	uint64		max_bytes;
	off_t		pos;
	int			wanted_compression;
	bool		settings_changed = false;

	if (aez_writer_failed)
		return false;

	wanted_compression = aez_select_compression();
	settings_changed = aez_fd >= 0 &&
		(aez_current_directory_guc != auto_explain_z_directory ||
		 aez_current_prefix_guc != auto_explain_z_file_prefix);
	if (settings_changed)
	{
		aez_close_file();
		aez_file_index = 0;
		aez_file_bytes = 0;
		aez_file_uncompressed_bytes = 0;
		aez_reset_templates();
		aez_invalidate_context_cache();
	}

	max_bytes = (uint64) auto_explain_z_max_file_size * 1024;
	if (aez_fd >= 0 &&
		((max_bytes > 0 && needed == 0 &&
		  aez_file_uncompressed_bytes >= max_bytes) ||
		 aez_file_compression != wanted_compression))
	{
		aez_close_file();
		aez_file_index++;
		aez_file_bytes = 0;
		aez_file_uncompressed_bytes = 0;
		aez_reset_templates();
		aez_invalidate_context_cache();
	}

	if (aez_fd >= 0)
		return true;

	if (MakePGDirectory(auto_explain_z_directory) < 0 && errno != EEXIST)
	{
		aez_writer_warning("could not create auto_explain_z binary log directory");
		return false;
	}

	path = psprintf("%s/%s-%ld-%d-" UINT64_FORMAT ".aez",
					auto_explain_z_directory,
					auto_explain_z_file_prefix,
					(long) MyStartTime,
					MyProcPid,
					aez_file_index);
	aez_file_compression = wanted_compression;
	aez_fd = BasicOpenFile(path, O_WRONLY | O_CREAT | O_APPEND | PG_BINARY);
	if (aez_fd < 0)
	{
		aez_writer_warning("could not open auto_explain_z binary log file");
		pfree(path);
		return false;
	}
	ReserveExternalFD();

	pos = lseek(aez_fd, 0, SEEK_END);
	if (pos < 0)
	{
		aez_close_file();
		aez_writer_warning("could not seek auto_explain_z binary log file");
		pfree(path);
		return false;
	}
	aez_file_bytes = (uint64) pos;
	aez_file_uncompressed_bytes = 0;

	if (aez_file_bytes == 0)
	{
		initStringInfo(&header);
		aez_build_file_header(&header);
		if (!aez_write_all(aez_fd, header.data, header.len))
		{
			aez_close_file();
			aez_writer_warning("could not write auto_explain_z binary log file header");
			pfree(path);
			return false;
		}
		aez_file_bytes += header.len;
	}
	else if (aez_file_compression != AEZ_COMPRESSION_NONE)
	{
		aez_close_file();
		aez_writer_warning("cannot append to an existing compressed auto_explain_z binary log file");
		pfree(path);
		return false;
	}

	if (!aez_start_compression())
	{
		aez_close_file();
		aez_writer_warning("could not initialize auto_explain_z file compression");
		pfree(path);
		return false;
	}

	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		if (aez_current_path)
			pfree(aez_current_path);
		aez_current_path = pstrdup(path);
		if (aez_current_directory)
			pfree(aez_current_directory);
		aez_current_directory = pstrdup(auto_explain_z_directory);
		if (aez_current_prefix)
			pfree(aez_current_prefix);
		aez_current_prefix = pstrdup(auto_explain_z_file_prefix);
		aez_current_directory_guc = auto_explain_z_directory;
		aez_current_prefix_guc = auto_explain_z_file_prefix;
		MemoryContextSwitchTo(oldcxt);
	}
	aez_cleanup_logs(false);

	pfree(path);
	return true;
}

static void
aez_close_file(void)
{
	if (aez_fd >= 0)
	{
		(void) aez_flush_pending();
		(void) aez_finish_compression();
		(void) close(aez_fd);
		aez_fd = -1;
		ReleaseExternalFD();
	}
}

static int
aez_select_compression(void)
{
	switch ((AezCompression) auto_explain_z_compression)
	{
		case AEZ_COMPRESSION_LZ4:
#ifdef USE_LZ4
			return AEZ_COMPRESSION_LZ4;
#else
			return AEZ_COMPRESSION_NONE;
#endif
		case AEZ_COMPRESSION_ZSTD:
#ifdef USE_ZSTD
			return AEZ_COMPRESSION_ZSTD;
#else
			return AEZ_COMPRESSION_NONE;
#endif
		case AEZ_COMPRESSION_NONE:
			break;
	}
	return AEZ_COMPRESSION_NONE;
}

static bool
aez_start_compression(void)
{
	switch ((AezCompression) aez_file_compression)
	{
		case AEZ_COMPRESSION_LZ4:
#ifdef USE_LZ4
			{
				LZ4F_preferences_t prefs;
				char		header[AEZ_COMPRESSION_WORK_BUFFER_SIZE];
				size_t		written;

				if (aez_lz4f_cctx != NULL)
					LZ4F_freeCompressionContext(aez_lz4f_cctx);
				if (LZ4F_isError(LZ4F_createCompressionContext(&aez_lz4f_cctx,
																LZ4F_VERSION)))
					return false;

				memset(&prefs, 0, sizeof(prefs));
				prefs.compressionLevel = 0;
				prefs.frameInfo.blockMode = LZ4F_blockLinked;
				written = LZ4F_compressBegin(aez_lz4f_cctx, header,
											 sizeof(header), &prefs);
				if (LZ4F_isError(written))
					return false;
				if (written > 0 && !aez_write_all(aez_fd, header, written))
					return false;
				aez_file_bytes += written;
				return true;
			}
#else
			return false;
#endif

		case AEZ_COMPRESSION_ZSTD:
#ifdef USE_ZSTD
			if (aez_zstd_cstream != NULL)
				ZSTD_freeCStream(aez_zstd_cstream);
			aez_zstd_cstream = ZSTD_createCStream();
			if (aez_zstd_cstream == NULL)
				return false;
			if (ZSTD_isError(ZSTD_initCStream(aez_zstd_cstream,
											  auto_explain_z_zstd_level)))
				return false;
			aez_zstd_out_size = ZSTD_CStreamOutSize();
			if (aez_zstd_out == NULL)
				aez_zstd_out = MemoryContextAlloc(TopMemoryContext,
												  aez_zstd_out_size);
			return true;
#else
			return false;
#endif

		case AEZ_COMPRESSION_NONE:
			return true;
	}
	return false;
}

static bool
aez_finish_compression(void)
{
	switch ((AezCompression) aez_file_compression)
	{
		case AEZ_COMPRESSION_LZ4:
#ifdef USE_LZ4
			if (aez_lz4f_cctx != NULL)
			{
				char		out[AEZ_COMPRESSION_WORK_BUFFER_SIZE];
				size_t		written;

				written = LZ4F_compressEnd(aez_lz4f_cctx, out, sizeof(out),
										   NULL);
				if (!LZ4F_isError(written) && written > 0)
				{
					if (!aez_write_all(aez_fd, out, written))
						return false;
					aez_file_bytes += written;
				}
				LZ4F_freeCompressionContext(aez_lz4f_cctx);
				aez_lz4f_cctx = NULL;
				return !LZ4F_isError(written);
			}
#endif
			return true;

		case AEZ_COMPRESSION_ZSTD:
#ifdef USE_ZSTD
			if (aez_zstd_cstream != NULL)
			{
				ZSTD_inBuffer input = {NULL, 0, 0};
				size_t		remaining;

				do
				{
					ZSTD_outBuffer output = {aez_zstd_out,
						aez_zstd_out_size, 0};

					remaining = ZSTD_compressStream2(aez_zstd_cstream,
													 &output, &input,
													 ZSTD_e_end);
					if (ZSTD_isError(remaining))
						return false;
					if (output.pos > 0)
					{
						if (!aez_write_all(aez_fd, aez_zstd_out, output.pos))
							return false;
						aez_file_bytes += output.pos;
					}
				} while (remaining != 0);

				ZSTD_freeCStream(aez_zstd_cstream);
				aez_zstd_cstream = NULL;
			}
#endif
			return true;

		case AEZ_COMPRESSION_NONE:
			return true;
	}
	return false;
}

static void
aez_init_pending_buffer(void)
{
	MemoryContext oldcxt;

	if (aez_pending_initialized)
		return;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	initStringInfo(&aez_pending);
	MemoryContextSwitchTo(oldcxt);
	aez_pending_initialized = true;
}

static bool
aez_flush_pending(void)
{
	if (!aez_pending_initialized || aez_pending.len == 0)
		return true;

	if (!aez_write_file_data(aez_pending.data, aez_pending.len))
		return false;

	resetStringInfo(&aez_pending);
	return true;
}

static bool
aez_write_file_data(const char *data, size_t len)
{
	switch ((AezCompression) aez_file_compression)
	{
		case AEZ_COMPRESSION_LZ4:
#ifdef USE_LZ4
			{
				size_t		bound;
				char	   *out;
				size_t		written;

				if (aez_lz4f_cctx == NULL)
					return false;

				bound = LZ4F_compressBound(len, NULL);
				if (bound == 0 || bound > MaxAllocSize)
					return false;
				out = palloc(bound);
				written = LZ4F_compressUpdate(aez_lz4f_cctx, out, bound,
											  data, len, NULL);
				if (LZ4F_isError(written))
				{
					pfree(out);
					return false;
				}
				if (written > 0 && !aez_write_all(aez_fd, out, written))
				{
					pfree(out);
					return false;
				}
				aez_file_bytes += written;
				pfree(out);
				return true;
			}
#else
			return false;
#endif

		case AEZ_COMPRESSION_ZSTD:
#ifdef USE_ZSTD
			{
				ZSTD_inBuffer input = {data, len, 0};

				if (aez_zstd_cstream == NULL || aez_zstd_out == NULL)
					return false;

				while (input.pos < input.size)
				{
					ZSTD_outBuffer output = {aez_zstd_out,
						aez_zstd_out_size, 0};
					size_t		rc;

					rc = ZSTD_compressStream2(aez_zstd_cstream, &output,
											  &input, ZSTD_e_continue);
					if (ZSTD_isError(rc))
						return false;
					if (output.pos > 0)
					{
						if (!aez_write_all(aez_fd, aez_zstd_out, output.pos))
							return false;
						aez_file_bytes += output.pos;
					}
				}
				return true;
			}
#else
			return false;
#endif

		case AEZ_COMPRESSION_NONE:
			if (!aez_write_all(aez_fd, data, len))
				return false;
			aez_file_bytes += len;
			return true;
	}
	return false;
}

static void
aez_cleanup_logs(bool force)
{
	DIR		   *dir;
	struct dirent *de;
	AezLogFile *files = NULL;
	int			nfiles = 0;
	int			allocated = 0;
	uint64		total_size = 0;
	TimestampTz now;
	uint64		max_size;
	int			remaining_files;

	if (auto_explain_z_retention_max_files <= 0 &&
		auto_explain_z_retention_max_size <= 0)
		return;

	now = GetCurrentTimestamp();
	if (!force &&
		aez_last_cleanup_ts != 0 &&
		auto_explain_z_retention_cleanup_interval > 0 &&
		!TimestampDifferenceExceeds(aez_last_cleanup_ts, now,
									auto_explain_z_retention_cleanup_interval))
		return;
	aez_last_cleanup_ts = now;

	dir = AllocateDir(auto_explain_z_directory);
	if (dir == NULL)
		return;

	while ((de = ReadDir(dir, auto_explain_z_directory)) != NULL)
	{
		char	   *path;
		struct stat st;
		AezLogFile *file;

		if (!aez_log_filename_matches(de->d_name))
			continue;

		path = psprintf("%s/%s", auto_explain_z_directory, de->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		{
			pfree(path);
			continue;
		}

		if (nfiles == allocated)
		{
			allocated = allocated ? allocated * 2 : 16;
			files = files ?
				repalloc(files, allocated * sizeof(AezLogFile)) :
				palloc(allocated * sizeof(AezLogFile));
		}

		file = &files[nfiles++];
		file->path = path;
		file->size = (uint64) st.st_size;
		file->mtime = time_t_to_timestamptz(st.st_mtime);
		file->is_current = aez_current_path != NULL &&
			strcmp(path, aez_current_path) == 0;
		total_size += file->size;
	}
	FreeDir(dir);

	if (nfiles <= 0)
		return;

	qsort(files, nfiles, sizeof(AezLogFile), aez_log_file_cmp);
	max_size = (uint64) auto_explain_z_retention_max_size * 1024;
	remaining_files = nfiles;

	for (int i = 0; i < nfiles; i++)
	{
		bool		over_files;
		bool		over_size;

		over_files = auto_explain_z_retention_max_files > 0 &&
			remaining_files > auto_explain_z_retention_max_files;
		over_size = max_size > 0 && total_size > max_size;

		if (!over_files && !over_size)
			break;
		if (files[i].is_current)
			continue;

		if (unlink(files[i].path) == 0)
		{
			total_size -= files[i].size;
			remaining_files--;
		}
		else if (auto_explain_z_report_writer_errors)
			ereport(WARNING,
					(errmsg_internal("could not remove old auto_explain_z binary log file \"%s\": %m",
									 files[i].path),
					 errhidestmt(true)));
	}
}

static int
aez_log_file_cmp(const void *a, const void *b)
{
	const AezLogFile *fa = (const AezLogFile *) a;
	const AezLogFile *fb = (const AezLogFile *) b;

	if (fa->mtime < fb->mtime)
		return -1;
	if (fa->mtime > fb->mtime)
		return 1;
	return strcmp(fa->path, fb->path);
}

static bool
aez_log_filename_matches(const char *name)
{
	Size		prefix_len;

	if (!aez_has_suffix(name, ".aez"))
		return false;

	prefix_len = strlen(auto_explain_z_file_prefix);
	return strncmp(name, auto_explain_z_file_prefix, prefix_len) == 0 &&
		name[prefix_len] == '-';
}

static bool
aez_has_suffix(const char *name, const char *suffix)
{
	Size		name_len = strlen(name);
	Size		suffix_len = strlen(suffix);

	return name_len >= suffix_len &&
		strcmp(name + name_len - suffix_len, suffix) == 0;
}

static void
aez_build_file_header(StringInfo buf)
{
	aez_put_u32(buf, AEZ_FILE_MAGIC);
	aez_put_u16(buf, AEZ_FORMAT_VERSION);
	aez_put_u16(buf, AEZ_FILE_HEADER_LEN);
	aez_put_u16(buf, (uint16) aez_file_compression);
	aez_put_u16(buf, 0);		/* file flags */
	aez_put_u32(buf, PG_VERSION_NUM);
	aez_put_i64(buf, MyStartTimestamp);
	aez_put_i64(buf, (int64) MyStartTime);
	aez_put_u32(buf, (uint32) MyProcPid);
	aez_put_u32(buf, 0);		/* reserved */
}

static bool
aez_write_all(int fd, const char *data, size_t len)
{
	const char *p = data;

	while (len > 0)
	{
		ssize_t		written;

		written = write(fd, p, len);
		if (written < 0)
		{
			if (errno == EINTR)
				continue;
			return false;
		}
		if (written == 0)
			return false;

		p += written;
		len -= written;
	}

	return true;
}

static void
aez_writer_warning(const char *message)
{
	if (!aez_writer_failed)
	{
		if (auto_explain_z_report_writer_errors)
			ereport(WARNING,
					(errmsg_internal("%s: %m", message),
					 errhidestmt(true)));
	}
	aez_writer_failed = true;
}

static void
aez_reset_top_stringinfo(StringInfoData *buf, bool *initialized)
{
	if (!*initialized)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		initStringInfo(buf);
		MemoryContextSwitchTo(oldcxt);
		*initialized = true;
	}
	else
		resetStringInfo(buf);
}

static bool
aez_prescan_node(PlanState *planstate, Bitmapset **rels_used)
{
	Plan	   *plan = planstate->plan;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
			*rels_used = bms_add_member(*rels_used,
										((Scan *) plan)->scanrelid);
			break;
		case T_ForeignScan:
			*rels_used = bms_add_members(*rels_used,
										 ((ForeignScan *) plan)->fs_base_relids);
			break;
		case T_CustomScan:
			*rels_used = bms_add_members(*rels_used,
										 ((CustomScan *) plan)->custom_relids);
			break;
		case T_ModifyTable:
			*rels_used = bms_add_member(*rels_used,
										((ModifyTable *) plan)->nominalRelation);
			if (((ModifyTable *) plan)->exclRelRTI)
				*rels_used = bms_add_member(*rels_used,
											((ModifyTable *) plan)->exclRelRTI);
			if (plan->targetlist)
				*rels_used = bms_add_member(*rels_used,
											linitial_int(((ModifyTable *) plan)->resultRelations));
			break;
		case T_Append:
			*rels_used = bms_add_members(*rels_used,
										 ((Append *) plan)->apprelids);
			break;
		case T_MergeAppend:
			*rels_used = bms_add_members(*rels_used,
										 ((MergeAppend *) plan)->apprelids);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, aez_prescan_node, rels_used);
}

static void
aez_init_serialize_state(AezSerializeState *state, QueryDesc *queryDesc,
						 uint32 payload_flags)
{
	ListCell   *lc;

	memset(state, 0, sizeof(*state));
	state->queryDesc = queryDesc;
	state->rtable = queryDesc->plannedstmt->rtable;
	state->verbose = (payload_flags & AEZ_QUERY_FLAG_VERBOSE) != 0;
	state->analyze = (payload_flags & AEZ_QUERY_FLAG_ANALYZE) != 0;
	state->rtable_size = list_length(state->rtable);
	foreach(lc, state->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind == RTE_GROUP)
		{
			state->rtable_size--;
			break;
		}
	}
}

static void
aez_init_deparse_state(AezSerializeState *state)
{
	Bitmapset  *rels_used = NULL;

	if (state->deparse_ready)
		return;

	aez_prescan_node(state->queryDesc->planstate, &rels_used);
	state->rtable_names = select_rtable_names_for_explain(state->rtable,
														 rels_used);
	state->deparse_cxt = deparse_context_for_plan_tree(state->queryDesc->plannedstmt,
													   state->rtable_names);
	state->deparse_ready = true;
}

static bool
aez_plan_is_disabled(Plan *plan)
{
	int			child_disabled_nodes = 0;

	if (plan->disabled_nodes == 0)
		return false;

	if (IsA(plan, Append))
	{
		ListCell   *lc;
		Append	   *aplan = (Append *) plan;

		foreach(lc, aplan->appendplans)
			child_disabled_nodes += ((Plan *) lfirst(lc))->disabled_nodes;
	}
	else if (IsA(plan, MergeAppend))
	{
		ListCell   *lc;
		MergeAppend *maplan = (MergeAppend *) plan;

		foreach(lc, maplan->mergeplans)
			child_disabled_nodes += ((Plan *) lfirst(lc))->disabled_nodes;
	}
	else if (IsA(plan, SubqueryScan))
		child_disabled_nodes += ((SubqueryScan *) plan)->subplan->disabled_nodes;
	else if (IsA(plan, CustomScan))
	{
		ListCell   *lc;
		CustomScan *cplan = (CustomScan *) plan;

		foreach(lc, cplan->custom_plans)
			child_disabled_nodes += ((Plan *) lfirst(lc))->disabled_nodes;
	}
	else
	{
		if (outerPlan(plan))
			child_disabled_nodes += outerPlan(plan)->disabled_nodes;
		if (innerPlan(plan))
			child_disabled_nodes += innerPlan(plan)->disabled_nodes;
	}

	return plan->disabled_nodes > child_disabled_nodes;
}

static void
aez_serialize_plan_node(StringInfo buf, AezSerializeState *state,
						PlanState *planstate, AezPlanRelationship relationship,
						List *ancestors)
{
	Plan	   *plan = planstate->plan;
	uint16		node_flags = 0;
	uint8		extra_kind = 0;
	uint32		extra1 = 0;
	uint32		extra2 = 0;
	int			child_count;
	uint8		extra1_code;
	uint8		extra2_code;
	uint8		width_code;
	uint8		child_count_code;
	uint8		size_flags;
	StringInfoData details;
	int			detail_count = 0;

	if (plan->parallel_aware)
		node_flags |= AEZ_NODE_FLAG_PARALLEL_AWARE;
	if (plan->async_capable)
		node_flags |= AEZ_NODE_FLAG_ASYNC_CAPABLE;
	if (aez_plan_is_disabled(plan))
		node_flags |= AEZ_NODE_FLAG_DISABLED;

	if (planstate->instrument)
		InstrEndLoop(planstate->instrument);

	if (auto_explain_z_log_analyze && planstate->instrument)
	{
		if (planstate->instrument->nloops > 0)
			node_flags |= AEZ_NODE_FLAG_HAS_ACTUAL;
		else
			node_flags |= AEZ_NODE_FLAG_NEVER_EXECUTED;

		if (auto_explain_z_log_buffers)
			node_flags |= AEZ_NODE_FLAG_HAS_BUFFERS;
		if (auto_explain_z_log_wal)
			node_flags |= AEZ_NODE_FLAG_HAS_WAL;
	}

	switch (nodeTag(plan))
	{
		case T_ModifyTable:
			extra_kind = AEZ_EXTRA_MODIFY_OPERATION;
			extra1 = (uint32) ((ModifyTable *) plan)->operation;
			break;
		case T_ForeignScan:
			extra_kind = AEZ_EXTRA_FOREIGN_OPERATION;
			extra1 = (uint32) ((ForeignScan *) plan)->operation;
			break;
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			extra_kind = AEZ_EXTRA_JOIN_TYPE;
			extra1 = (uint32) ((Join *) plan)->jointype;
			break;
		case T_Agg:
			extra_kind = AEZ_EXTRA_AGG;
			extra1 = (uint32) ((Agg *) plan)->aggstrategy;
			extra2 = (uint32) ((Agg *) plan)->aggsplit;
			break;
		case T_SetOp:
			extra_kind = AEZ_EXTRA_SETOP;
			extra1 = (uint32) ((SetOp *) plan)->strategy;
			extra2 = (uint32) ((SetOp *) plan)->cmd;
			break;
		case T_IndexScan:
			extra_kind = AEZ_EXTRA_INDEX_SCAN_DIRECTION;
			extra1 = (uint32) (((IndexScan *) plan)->indexorderdir + 1);
			break;
		case T_IndexOnlyScan:
			extra_kind = AEZ_EXTRA_INDEX_SCAN_DIRECTION;
			extra1 = (uint32) (((IndexOnlyScan *) plan)->indexorderdir + 1);
			break;
		default:
			break;
	}

	child_count = aez_count_plan_children(planstate);
	extra1_code = aez_uint_size_code(extra1);
	extra2_code = aez_uint_size_code(extra2);
	width_code = aez_int_size_code(plan->plan_width);
	child_count_code = aez_uint_size_code((uint64) child_count);
	size_flags = extra1_code |
		(extra2_code << 2) |
		(width_code << 4) |
		(child_count_code << 6);

	aez_put_u8(buf, (uint8) relationship);
	aez_put_u16(buf, (uint16) nodeTag(plan));
	aez_put_u16(buf, node_flags);
	aez_put_u8(buf, extra_kind);
	aez_put_u8(buf, size_flags);
	aez_put_uint_sized(buf, extra1, extra1_code);
	aez_put_uint_sized(buf, extra2, extra2_code);
	aez_put_double(buf, plan->startup_cost);
	aez_put_double(buf, plan->total_cost);
	aez_put_double(buf, plan->plan_rows);
	aez_put_int_sized(buf, plan->plan_width, width_code);

	if (node_flags & AEZ_NODE_FLAG_HAS_ACTUAL)
	{
		Instrumentation *instr = planstate->instrument;
		double		nloops = instr->nloops;

		aez_put_i64(buf, aez_explain_time_microseconds(instr->startup / nloops));
		aez_put_i64(buf, aez_explain_time_microseconds(instr->total / nloops));
		aez_put_double(buf, instr->ntuples / nloops);
		aez_put_double(buf, nloops);
	}

	if (node_flags & AEZ_NODE_FLAG_HAS_BUFFERS)
	{
		BufferUsage *usage = &planstate->instrument->bufusage;

		aez_put_i64(buf, usage->shared_blks_hit);
		aez_put_i64(buf, usage->shared_blks_read);
		aez_put_i64(buf, usage->shared_blks_dirtied);
		aez_put_i64(buf, usage->shared_blks_written);
		aez_put_i64(buf, usage->local_blks_hit);
		aez_put_i64(buf, usage->local_blks_read);
		aez_put_i64(buf, usage->local_blks_dirtied);
		aez_put_i64(buf, usage->local_blks_written);
		aez_put_i64(buf, usage->temp_blks_read);
		aez_put_i64(buf, usage->temp_blks_written);
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->shared_blk_read_time));
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->shared_blk_write_time));
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->local_blk_read_time));
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->local_blk_write_time));
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->temp_blk_read_time));
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->temp_blk_write_time));
	}

	if (node_flags & AEZ_NODE_FLAG_HAS_WAL)
	{
		WalUsage   *usage = &planstate->instrument->walusage;

		aez_put_i64(buf, usage->wal_records);
		aez_put_i64(buf, usage->wal_fpi);
		aez_put_u64(buf, usage->wal_bytes);
		aez_put_i64(buf, usage->wal_buffers_full);
	}

	aez_append_plan_identity(buf, state, plan);

	initStringInfo(&details);
	if (auto_explain_z_profile == AEZ_PROFILE_FULL)
		aez_serialize_node_details(&details, &detail_count, state, planstate,
								   ancestors);
	aez_serialize_structural_node_details(&details, &detail_count, planstate);
	aez_serialize_extension_explain_text(&details, &detail_count, state,
										 planstate, ancestors);
	aez_put_u16(buf, (uint16) detail_count);
	appendBinaryStringInfo(buf, details.data, details.len);

	aez_put_uint_sized(buf, (uint64) child_count, child_count_code);
	aez_serialize_plan_children(buf, state, planstate, ancestors);
}

static int
aez_count_plan_children(PlanState *planstate)
{
	Plan	   *plan = planstate->plan;
	int			nchildren = 0;

	nchildren += list_length(planstate->initPlan);
	if (outerPlanState(planstate))
		nchildren++;
	if (innerPlanState(planstate))
		nchildren++;

	switch (nodeTag(plan))
	{
		case T_Append:
			nchildren += ((AppendState *) planstate)->as_nplans;
			break;
		case T_MergeAppend:
			nchildren += ((MergeAppendState *) planstate)->ms_nplans;
			break;
		case T_BitmapAnd:
			nchildren += ((BitmapAndState *) planstate)->nplans;
			break;
		case T_BitmapOr:
			nchildren += ((BitmapOrState *) planstate)->nplans;
			break;
		case T_SubqueryScan:
			nchildren++;
			break;
		case T_CustomScan:
			nchildren += list_length(((CustomScanState *) planstate)->custom_ps);
			break;
		default:
			break;
	}

	nchildren += list_length(planstate->subPlan);
	return nchildren;
}

static void
aez_serialize_plan_children(StringInfo buf, AezSerializeState *state,
							PlanState *planstate, List *ancestors)
{
	Plan	   *plan = planstate->plan;
	List	   *child_ancestors;

	child_ancestors = lcons(plan, ancestors);

	aez_serialize_subplans(buf, state, planstate->initPlan,
						   AEZ_PLAN_REL_INITPLAN, child_ancestors);

	if (outerPlanState(planstate))
		aez_serialize_plan_node(buf, state, outerPlanState(planstate),
								AEZ_PLAN_REL_OUTER, child_ancestors);
	if (innerPlanState(planstate))
		aez_serialize_plan_node(buf, state, innerPlanState(planstate),
								AEZ_PLAN_REL_INNER, child_ancestors);

	switch (nodeTag(plan))
	{
		case T_Append:
			aez_serialize_plan_member_nodes(buf, state,
											((AppendState *) planstate)->appendplans,
											((AppendState *) planstate)->as_nplans,
											AEZ_PLAN_REL_MEMBER,
											child_ancestors);
			break;
		case T_MergeAppend:
			aez_serialize_plan_member_nodes(buf, state,
											((MergeAppendState *) planstate)->mergeplans,
											((MergeAppendState *) planstate)->ms_nplans,
											AEZ_PLAN_REL_MEMBER,
											child_ancestors);
			break;
		case T_BitmapAnd:
			aez_serialize_plan_member_nodes(buf, state,
											((BitmapAndState *) planstate)->bitmapplans,
											((BitmapAndState *) planstate)->nplans,
											AEZ_PLAN_REL_MEMBER,
											child_ancestors);
			break;
		case T_BitmapOr:
			aez_serialize_plan_member_nodes(buf, state,
											((BitmapOrState *) planstate)->bitmapplans,
											((BitmapOrState *) planstate)->nplans,
											AEZ_PLAN_REL_MEMBER,
											child_ancestors);
			break;
		case T_SubqueryScan:
			aez_serialize_plan_node(buf, state,
									((SubqueryScanState *) planstate)->subplan,
									AEZ_PLAN_REL_SUBQUERY, child_ancestors);
			break;
		case T_CustomScan:
			{
				ListCell   *lc;

				foreach(lc, ((CustomScanState *) planstate)->custom_ps)
					aez_serialize_plan_node(buf, state,
											(PlanState *) lfirst(lc),
											AEZ_PLAN_REL_CUSTOM,
											child_ancestors);
			}
			break;
		default:
			break;
	}

	aez_serialize_subplans(buf, state, planstate->subPlan,
						   AEZ_PLAN_REL_SUBPLAN, child_ancestors);

	child_ancestors = list_delete_first(child_ancestors);
}

static void
aez_serialize_plan_member_nodes(StringInfo buf, AezSerializeState *state,
								PlanState **planstates, int nplans,
								AezPlanRelationship relationship,
								List *ancestors)
{
	for (int i = 0; i < nplans; i++)
		aez_serialize_plan_node(buf, state, planstates[i], relationship,
								ancestors);
}

static void
aez_serialize_subplans(StringInfo buf, AezSerializeState *state, List *plans,
					   AezPlanRelationship relationship, List *ancestors)
{
	ListCell   *lc;

	foreach(lc, plans)
	{
		SubPlanState *sps = lfirst_node(SubPlanState, lc);

		aez_serialize_plan_node(buf, state, sps->planstate, relationship,
								ancestors);
	}
}

static void
aez_append_plan_identity(StringInfo buf, AezSerializeState *state, Plan *plan)
{
	uint8		obj_flags = 0;
	Oid			relid = InvalidOid;
	Oid			indexid = InvalidOid;
	const char *relname = NULL;
	const char *schemaname = NULL;
	const char *aliasname = NULL;
	const char *indexname = NULL;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_ForeignScan:
		case T_CustomScan:
			if (((Scan *) plan)->scanrelid > 0 &&
				((Scan *) plan)->scanrelid <= list_length(state->rtable))
			{
				RangeTblEntry *rte;

				rte = rt_fetch(((Scan *) plan)->scanrelid,
							   state->rtable);
				aliasname = rte->eref ? rte->eref->aliasname : NULL;
				if (rte->rtekind == RTE_RELATION)
				{
					relid = rte->relid;
					relname = get_rel_name(relid);
					if (state->verbose)
						schemaname = get_namespace_name_or_temp(get_rel_namespace(relid));
					obj_flags |= AEZ_OBJ_RELATION;
				}
			}
			break;
		case T_IndexScan:
			indexid = ((IndexScan *) plan)->indexid;
			indexname = get_rel_name(indexid);
			obj_flags |= AEZ_OBJ_INDEX;
			goto scan_relation;
		case T_IndexOnlyScan:
			indexid = ((IndexOnlyScan *) plan)->indexid;
			indexname = get_rel_name(indexid);
			obj_flags |= AEZ_OBJ_INDEX;
			goto scan_relation;
		case T_BitmapIndexScan:
			indexid = ((BitmapIndexScan *) plan)->indexid;
			indexname = get_rel_name(indexid);
			obj_flags |= AEZ_OBJ_INDEX;
			break;
		case T_ModifyTable:
			if (((ModifyTable *) plan)->nominalRelation > 0 &&
				((ModifyTable *) plan)->nominalRelation <= list_length(state->rtable))
			{
				Index		rtindex = ((ModifyTable *) plan)->nominalRelation;
				RangeTblEntry *rte;

				rte = rt_fetch(rtindex, state->rtable);
				aliasname = rte->eref ? rte->eref->aliasname : NULL;
				if (rte->rtekind == RTE_RELATION)
				{
					relid = rte->relid;
					relname = get_rel_name(relid);
					if (state->verbose)
						schemaname = get_namespace_name_or_temp(get_rel_namespace(relid));
					obj_flags |= AEZ_OBJ_RELATION;
				}
			}
			break;
		default:
			break;
	}

	goto done;

scan_relation:
	if (((Scan *) plan)->scanrelid > 0 &&
		((Scan *) plan)->scanrelid <= list_length(state->rtable))
	{
		RangeTblEntry *rte;

		rte = rt_fetch(((Scan *) plan)->scanrelid,
					   state->rtable);
		aliasname = rte->eref ? rte->eref->aliasname : NULL;
		if (rte->rtekind == RTE_RELATION)
		{
			relid = rte->relid;
			relname = get_rel_name(relid);
			if (state->verbose)
				schemaname = get_namespace_name_or_temp(get_rel_namespace(relid));
			obj_flags |= AEZ_OBJ_RELATION;
		}
	}

done:
	{
		uint8		relid_code = aez_uint_size_code(relid);
		uint8		indexid_code = aez_uint_size_code(indexid);
		uint8		ctrl = obj_flags |
			(relid_code << 2) |
			(indexid_code << 4);

		aez_put_u8(buf, ctrl);
	}
	if (obj_flags & AEZ_OBJ_RELATION)
	{
		aez_put_uint_sized(buf, relid, aez_uint_size_code(relid));
		aez_put_string(buf, relname, -1);
		aez_put_string(buf, aliasname, -1);
		aez_put_string(buf, schemaname, -1);
	}
	if (obj_flags & AEZ_OBJ_INDEX)
	{
		aez_put_uint_sized(buf, indexid, aez_uint_size_code(indexid));
		aez_put_string(buf, indexname, -1);
	}
}

static void
aez_serialize_plan_metrics(StringInfo buf, AezSerializeState *state,
						   PlanState *planstate, List *ancestors)
{
	Plan	   *plan = planstate->plan;
	uint16		node_flags = 0;
	int			child_count;
	uint8		width_code;
	uint8		child_count_code;
	uint8		size_flags;
	StringInfoData details;
	int			detail_count = 0;

	if (plan->parallel_aware)
		node_flags |= AEZ_NODE_FLAG_PARALLEL_AWARE;
	if (plan->async_capable)
		node_flags |= AEZ_NODE_FLAG_ASYNC_CAPABLE;
	if (aez_plan_is_disabled(plan))
		node_flags |= AEZ_NODE_FLAG_DISABLED;

	if (planstate->instrument)
		InstrEndLoop(planstate->instrument);

	if (state->analyze && planstate->instrument)
	{
		if (planstate->instrument->nloops > 0)
			node_flags |= AEZ_NODE_FLAG_HAS_ACTUAL;
		else
			node_flags |= AEZ_NODE_FLAG_NEVER_EXECUTED;

		if (auto_explain_z_log_buffers)
			node_flags |= AEZ_NODE_FLAG_HAS_BUFFERS;
		if (auto_explain_z_log_wal)
			node_flags |= AEZ_NODE_FLAG_HAS_WAL;
	}

	child_count = aez_count_plan_children(planstate);
	width_code = aez_int_size_code(plan->plan_width);
	child_count_code = aez_uint_size_code((uint64) child_count);
	size_flags = width_code | (child_count_code << 2);

	aez_put_u16(buf, (uint16) nodeTag(plan));
	aez_put_u16(buf, node_flags);
	aez_put_u8(buf, size_flags);
	aez_put_double(buf, plan->startup_cost);
	aez_put_double(buf, plan->total_cost);
	aez_put_double(buf, plan->plan_rows);
	aez_put_int_sized(buf, plan->plan_width, width_code);

	if (node_flags & AEZ_NODE_FLAG_HAS_ACTUAL)
	{
		Instrumentation *instr = planstate->instrument;
		double		nloops = instr->nloops;

		aez_put_i64(buf, aez_explain_time_microseconds(instr->startup / nloops));
		aez_put_i64(buf, aez_explain_time_microseconds(instr->total / nloops));
		aez_put_double(buf, instr->ntuples / nloops);
		aez_put_double(buf, nloops);
	}

	if (node_flags & AEZ_NODE_FLAG_HAS_BUFFERS)
	{
		BufferUsage *usage = &planstate->instrument->bufusage;

		aez_put_i64(buf, usage->shared_blks_hit);
		aez_put_i64(buf, usage->shared_blks_read);
		aez_put_i64(buf, usage->shared_blks_dirtied);
		aez_put_i64(buf, usage->shared_blks_written);
		aez_put_i64(buf, usage->local_blks_hit);
		aez_put_i64(buf, usage->local_blks_read);
		aez_put_i64(buf, usage->local_blks_dirtied);
		aez_put_i64(buf, usage->local_blks_written);
		aez_put_i64(buf, usage->temp_blks_read);
		aez_put_i64(buf, usage->temp_blks_written);
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->shared_blk_read_time));
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->shared_blk_write_time));
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->local_blk_read_time));
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->local_blk_write_time));
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->temp_blk_read_time));
		aez_put_i64(buf, INSTR_TIME_GET_MICROSEC(usage->temp_blk_write_time));
	}

	if (node_flags & AEZ_NODE_FLAG_HAS_WAL)
	{
		WalUsage   *usage = &planstate->instrument->walusage;

		aez_put_i64(buf, usage->wal_records);
		aez_put_i64(buf, usage->wal_fpi);
		aez_put_u64(buf, usage->wal_bytes);
		aez_put_i64(buf, usage->wal_buffers_full);
	}

	initStringInfo(&details);
	if (auto_explain_z_profile == AEZ_PROFILE_FULL)
		aez_serialize_dynamic_node_details(&details, &detail_count, state,
										   planstate);
	aez_serialize_extension_explain_text(&details, &detail_count, state,
										 planstate, ancestors);
	aez_put_u16(buf, (uint16) detail_count);
	appendBinaryStringInfo(buf, details.data, details.len);

	aez_put_uint_sized(buf, (uint64) child_count, child_count_code);
	aez_serialize_plan_metric_children(buf, state, planstate, ancestors);
}

static void
aez_serialize_plan_metric_children(StringInfo buf, AezSerializeState *state,
								   PlanState *planstate, List *ancestors)
{
	Plan	   *plan = planstate->plan;
	List	   *child_ancestors;
	ListCell   *lc;

	child_ancestors = lcons(plan, ancestors);

	foreach(lc, planstate->initPlan)
	{
		SubPlanState *sps = lfirst_node(SubPlanState, lc);

		aez_serialize_plan_metrics(buf, state, sps->planstate,
								   child_ancestors);
	}

	if (outerPlanState(planstate))
		aez_serialize_plan_metrics(buf, state, outerPlanState(planstate),
								   child_ancestors);
	if (innerPlanState(planstate))
		aez_serialize_plan_metrics(buf, state, innerPlanState(planstate),
								   child_ancestors);

	switch (nodeTag(plan))
	{
		case T_Append:
			for (int i = 0; i < ((AppendState *) planstate)->as_nplans; i++)
				aez_serialize_plan_metrics(buf, state,
										   ((AppendState *) planstate)->appendplans[i],
										   child_ancestors);
			break;
		case T_MergeAppend:
			for (int i = 0; i < ((MergeAppendState *) planstate)->ms_nplans; i++)
				aez_serialize_plan_metrics(buf, state,
										   ((MergeAppendState *) planstate)->mergeplans[i],
										   child_ancestors);
			break;
		case T_BitmapAnd:
			for (int i = 0; i < ((BitmapAndState *) planstate)->nplans; i++)
				aez_serialize_plan_metrics(buf, state,
										   ((BitmapAndState *) planstate)->bitmapplans[i],
										   child_ancestors);
			break;
		case T_BitmapOr:
			for (int i = 0; i < ((BitmapOrState *) planstate)->nplans; i++)
				aez_serialize_plan_metrics(buf, state,
										   ((BitmapOrState *) planstate)->bitmapplans[i],
										   child_ancestors);
			break;
		case T_SubqueryScan:
			aez_serialize_plan_metrics(buf, state,
									   ((SubqueryScanState *) planstate)->subplan,
									   child_ancestors);
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScanState *) planstate)->custom_ps)
				aez_serialize_plan_metrics(buf, state,
										   (PlanState *) lfirst(lc),
										   child_ancestors);
			break;
		default:
			break;
	}

	foreach(lc, planstate->subPlan)
	{
		SubPlanState *sps = lfirst_node(SubPlanState, lc);

		aez_serialize_plan_metrics(buf, state, sps->planstate,
								   child_ancestors);
	}

	child_ancestors = list_delete_first(child_ancestors);
}

static void
aez_serialize_dynamic_node_details(StringInfo details, int *detail_count,
								   AezSerializeState *state,
								   PlanState *planstate)
{
	Plan	   *plan = planstate->plan;

	switch (nodeTag(plan))
	{
		case T_IndexScan:
			if (((IndexScan *) plan)->indexqualorig)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_INDEX_RECHECK,
												 2);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;
		case T_IndexOnlyScan:
			if (((IndexOnlyScan *) plan)->recheckqual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_INDEX_RECHECK,
												 2);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			if (state->analyze && planstate->instrument)
				aez_detail_double(details, detail_count,
								  AEZ_DETAIL_HEAP_FETCHES,
								  planstate->instrument->ntuples2);
			break;
		case T_BitmapHeapScan:
			if (((BitmapHeapScan *) plan)->bitmapqualorig)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_INDEX_RECHECK,
												 2);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;
		case T_NestLoop:
			if (((NestLoop *) plan)->join.joinqual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_JOIN_FILTER,
												 1);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 2);
			break;
		case T_MergeJoin:
			if (((MergeJoin *) plan)->join.joinqual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_JOIN_FILTER,
												 1);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 2);
			break;
		case T_HashJoin:
			if (((HashJoin *) plan)->join.joinqual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_JOIN_FILTER,
												 1);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 2);
			break;
		case T_Gather:
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			if (state->analyze)
				aez_detail_double(details, detail_count,
								  AEZ_DETAIL_WORKERS_LAUNCHED,
								  ((GatherState *) planstate)->nworkers_launched);
			break;
		case T_GatherMerge:
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			if (state->analyze)
				aez_detail_double(details, detail_count,
								  AEZ_DETAIL_WORKERS_LAUNCHED,
								  ((GatherMergeState *) planstate)->nworkers_launched);
			break;
		case T_Agg:
		case T_WindowAgg:
		case T_Group:
		case T_Result:
		case T_SeqScan:
		case T_SampleScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_ForeignScan:
		case T_CustomScan:
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;
		default:
			break;
	}

	aez_serialize_runtime_node_details(details, detail_count, state,
									   planstate, NIL, true);
}

static void
aez_serialize_runtime_node_details(StringInfo details, int *detail_count,
								   AezSerializeState *state,
								   PlanState *planstate,
								   List *ancestors,
								   bool dynamic_only)
{
	Plan	   *plan = planstate->plan;

	switch (nodeTag(plan))
	{
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			if (!dynamic_only)
				aez_detail_bool(details, detail_count,
								AEZ_DETAIL_INNER_UNIQUE,
								((Join *) plan)->inner_unique);
			break;
		default:
			break;
	}

	switch (nodeTag(plan))
	{
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapIndexScan:
			aez_detail_index_searches(details, detail_count, state, planstate);
			break;
		case T_BitmapHeapScan:
			aez_detail_bitmap_heap_blocks(details, detail_count, state,
										  (BitmapHeapScanState *) planstate);
			break;
		case T_Sort:
			aez_detail_sort_info(details, detail_count, state,
								 (SortState *) planstate);
			break;
		case T_IncrementalSort:
			aez_detail_incremental_sort_info(details, detail_count, state,
											 (IncrementalSortState *) planstate);
			break;
		case T_Hash:
			aez_detail_hash_info(details, detail_count,
								 (HashState *) planstate);
			break;
		case T_Material:
			aez_detail_storage_info(details, detail_count,
									AEZ_DETAIL_STORAGE_INFO,
									((MaterialState *) planstate)->tuplestorestate);
			break;
		case T_WindowAgg:
			aez_detail_storage_info(details, detail_count,
									AEZ_DETAIL_STORAGE_INFO,
									((WindowAggState *) planstate)->buffer);
			break;
		case T_CteScan:
			if (((CteScanState *) planstate)->leader)
				aez_detail_storage_info(details, detail_count,
										AEZ_DETAIL_STORAGE_INFO,
										((CteScanState *) planstate)->leader->cte_table);
			break;
		case T_TableFuncScan:
			aez_detail_storage_info(details, detail_count,
									AEZ_DETAIL_STORAGE_INFO,
									((TableFuncScanState *) planstate)->tupstore);
			break;
		case T_RecursiveUnion:
			if (state->analyze)
			{
				RecursiveUnionState *rstate = (RecursiveUnionState *) planstate;
				char	   *maxStorageType;
				char	   *tempStorageType;
				int64		maxSpaceUsed;
				int64		tempSpaceUsed;
				const char *values[2];
				char	   *space_kb;

				tuplestore_get_stats(rstate->working_table, &tempStorageType,
									 &tempSpaceUsed);
				tuplestore_get_stats(rstate->intermediate_table, &maxStorageType,
									 &maxSpaceUsed);
				if (tempSpaceUsed > maxSpaceUsed)
					maxStorageType = tempStorageType;
				maxSpaceUsed += tempSpaceUsed;
				space_kb = psprintf(INT64_FORMAT,
									 (int64) AEZ_BYTES_TO_KILOBYTES(maxSpaceUsed));
				values[0] = maxStorageType;
				values[1] = space_kb;
				aez_detail_cstring_array(details, detail_count,
										 AEZ_DETAIL_STORAGE_INFO, 2, values);
			}
			break;
		case T_Memoize:
			aez_detail_memoize_info(details, detail_count, state,
									(MemoizeState *) planstate, ancestors,
									dynamic_only);
			break;
		case T_Agg:
			aez_detail_hashagg_info(details, detail_count, state,
									(AggState *) planstate,
									dynamic_only);
			break;
		case T_Append:
			if (!dynamic_only &&
				list_length(((Append *) plan)->appendplans) >
				((AppendState *) planstate)->as_nplans)
				aez_detail_i64(details, detail_count,
							   AEZ_DETAIL_SUBPLANS_REMOVED,
							   list_length(((Append *) plan)->appendplans) -
							   ((AppendState *) planstate)->as_nplans);
			break;
		case T_MergeAppend:
			if (!dynamic_only &&
				list_length(((MergeAppend *) plan)->mergeplans) >
				((MergeAppendState *) planstate)->ms_nplans)
				aez_detail_i64(details, detail_count,
							   AEZ_DETAIL_SUBPLANS_REMOVED,
							   list_length(((MergeAppend *) plan)->mergeplans) -
							   ((MergeAppendState *) planstate)->ms_nplans);
			break;
		case T_ModifyTable:
			aez_detail_modifytable_info(details, detail_count, state,
										(ModifyTableState *) planstate,
										ancestors, dynamic_only);
			break;
		default:
			break;
	}
}

static void
aez_serialize_structural_node_details(StringInfo details, int *detail_count,
									  PlanState *planstate)
{
	Plan	   *plan = planstate->plan;

	switch (nodeTag(plan))
	{
		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;

				if (cscan->methods != NULL && cscan->methods->CustomName != NULL)
					aez_detail_string(details, detail_count,
									  AEZ_DETAIL_CUSTOM_PLAN_PROVIDER,
									  cscan->methods->CustomName);
			}
			break;
		case T_Gather:
			aez_detail_bool(details, detail_count, AEZ_DETAIL_SINGLE_COPY,
							((Gather *) plan)->single_copy);
			break;
		default:
			break;
	}
}

static void
aez_detail_sort_info(StringInfo details, int *detail_count,
					 AezSerializeState *state, SortState *sortstate)
{
	if (!state->analyze)
		return;

	if (sortstate->sort_Done && sortstate->tuplesortstate != NULL)
	{
		TuplesortInstrumentation stats;
		const char *values[4];
		char	   *worker = psprintf("%d", -1);
		char	   *space = NULL;

		tuplesort_get_stats((Tuplesortstate *) sortstate->tuplesortstate,
							&stats);
		space = psprintf(INT64_FORMAT, stats.spaceUsed);
		values[0] = worker;
		values[1] = tuplesort_method_name(stats.sortMethod);
		values[2] = tuplesort_space_type_name(stats.spaceType);
		values[3] = space;
		aez_detail_cstring_array(details, detail_count,
								 AEZ_DETAIL_SORT_INFO, 4, values);
	}

	if (sortstate->shared_info != NULL)
	{
		for (int n = 0; n < sortstate->shared_info->num_workers; n++)
		{
			TuplesortInstrumentation *sinstrument;
			const char *values[4];
			char	   *worker;
			char	   *space;

			sinstrument = &sortstate->shared_info->sinstrument[n];
			if (sinstrument->sortMethod == SORT_TYPE_STILL_IN_PROGRESS)
				continue;

			worker = psprintf("%d", n);
			space = psprintf(INT64_FORMAT, sinstrument->spaceUsed);
			values[0] = worker;
			values[1] = tuplesort_method_name(sinstrument->sortMethod);
			values[2] = tuplesort_space_type_name(sinstrument->spaceType);
			values[3] = space;
			aez_detail_cstring_array(details, detail_count,
									 AEZ_DETAIL_SORT_INFO, 4, values);
		}
	}
}

static void
aez_detail_incremental_sort_info(StringInfo details, int *detail_count,
								 AezSerializeState *state,
								 IncrementalSortState *incrsortstate)
{
	IncrementalSortGroupInfo *fullsortGroupInfo;
	IncrementalSortGroupInfo *prefixsortGroupInfo;

	if (!state->analyze)
		return;

	fullsortGroupInfo = &incrsortstate->incsort_info.fullsortGroupInfo;
	if (fullsortGroupInfo->groupCount > 0)
	{
		aez_detail_incremental_sort_group(details, detail_count, -1,
										  fullsortGroupInfo, "Full-sort");
		prefixsortGroupInfo = &incrsortstate->incsort_info.prefixsortGroupInfo;
		if (prefixsortGroupInfo->groupCount > 0)
			aez_detail_incremental_sort_group(details, detail_count, -1,
											  prefixsortGroupInfo, "Pre-sorted");
	}

	if (incrsortstate->shared_info != NULL)
	{
		for (int n = 0; n < incrsortstate->shared_info->num_workers; n++)
		{
			IncrementalSortInfo *incsort_info =
				&incrsortstate->shared_info->sinfo[n];

			fullsortGroupInfo = &incsort_info->fullsortGroupInfo;
			if (fullsortGroupInfo->groupCount == 0)
				continue;
			aez_detail_incremental_sort_group(details, detail_count, n,
											  fullsortGroupInfo, "Full-sort");
			prefixsortGroupInfo = &incsort_info->prefixsortGroupInfo;
			if (prefixsortGroupInfo->groupCount > 0)
				aez_detail_incremental_sort_group(details, detail_count, n,
												  prefixsortGroupInfo,
												  "Pre-sorted");
		}
	}
}

static void
aez_detail_incremental_sort_group(StringInfo details, int *detail_count,
								  int worker,
								  IncrementalSortGroupInfo *groupInfo,
								  const char *label)
{
	StringInfoData methods;
	const char *values[9];
	char	   *worker_s;
	char	   *groups;
	char	   *avg_mem;
	char	   *peak_mem;
	char	   *avg_disk;
	char	   *peak_disk;

	initStringInfo(&methods);
	for (int bit = 0; bit < NUM_TUPLESORTMETHODS; bit++)
	{
		TuplesortMethod sortMethod = (1 << bit);

		if (groupInfo->sortMethods & sortMethod)
		{
			if (methods.len > 0)
				appendStringInfoChar(&methods, ',');
			appendStringInfoString(&methods,
								   tuplesort_method_name(sortMethod));
		}
	}

	worker_s = psprintf("%d", worker);
	groups = psprintf(INT64_FORMAT, groupInfo->groupCount);
	avg_mem = psprintf(INT64_FORMAT,
					   groupInfo->groupCount > 0 ?
					   groupInfo->totalMemorySpaceUsed / groupInfo->groupCount : 0);
	peak_mem = psprintf(INT64_FORMAT, groupInfo->maxMemorySpaceUsed);
	avg_disk = psprintf(INT64_FORMAT,
						groupInfo->groupCount > 0 ?
						groupInfo->totalDiskSpaceUsed / groupInfo->groupCount : 0);
	peak_disk = psprintf(INT64_FORMAT, groupInfo->maxDiskSpaceUsed);

	values[0] = worker_s;
	values[1] = label;
	values[2] = groups;
	values[3] = methods.data;
	values[4] = tuplesort_space_type_name(SORT_SPACE_TYPE_MEMORY);
	values[5] = avg_mem;
	values[6] = peak_mem;
	values[7] = avg_disk;
	values[8] = peak_disk;
	aez_detail_cstring_array(details, detail_count,
							 AEZ_DETAIL_INCREMENTAL_SORT_GROUP, 9, values);
}

static void
aez_detail_hash_info(StringInfo details, int *detail_count,
					 HashState *hashstate)
{
	HashInstrumentation hinstrument = {0};
	const char *values[5];
	char	   *nbuckets;
	char	   *nbuckets_original;
	char	   *nbatch;
	char	   *nbatch_original;
	char	   *space_peak_kb;

	if (hashstate->hinstrument)
		memcpy(&hinstrument, hashstate->hinstrument,
			   sizeof(HashInstrumentation));

	if (hashstate->shared_info)
	{
		SharedHashInfo *shared_info = hashstate->shared_info;

		for (int i = 0; i < shared_info->num_workers; ++i)
		{
			HashInstrumentation *worker_hi = &shared_info->hinstrument[i];

			hinstrument.nbuckets = Max(hinstrument.nbuckets,
									   worker_hi->nbuckets);
			hinstrument.nbuckets_original = Max(hinstrument.nbuckets_original,
												worker_hi->nbuckets_original);
			hinstrument.nbatch = Max(hinstrument.nbatch,
									 worker_hi->nbatch);
			hinstrument.nbatch_original = Max(hinstrument.nbatch_original,
											  worker_hi->nbatch_original);
			hinstrument.space_peak = Max(hinstrument.space_peak,
										 worker_hi->space_peak);
		}
	}

	if (hinstrument.nbatch <= 0)
		return;

	nbuckets = psprintf("%d", hinstrument.nbuckets);
	nbuckets_original = psprintf("%d", hinstrument.nbuckets_original);
	nbatch = psprintf("%d", hinstrument.nbatch);
	nbatch_original = psprintf("%d", hinstrument.nbatch_original);
	space_peak_kb = psprintf(UINT64_FORMAT,
							 (uint64) AEZ_BYTES_TO_KILOBYTES(hinstrument.space_peak));
	values[0] = nbuckets;
	values[1] = nbuckets_original;
	values[2] = nbatch;
	values[3] = nbatch_original;
	values[4] = space_peak_kb;
	aez_detail_cstring_array(details, detail_count,
							 AEZ_DETAIL_HASH_INFO, 5, values);
}

static void
aez_detail_storage_info(StringInfo details, int *detail_count,
						AezDetailCode code, Tuplestorestate *tupstore)
{
	char	   *storage_type;
	int64		space_used;
	char	   *space_kb;
	const char *values[2];

	if (tupstore == NULL)
		return;

	tuplestore_get_stats(tupstore, &storage_type, &space_used);
	space_kb = psprintf(INT64_FORMAT,
						 (int64) AEZ_BYTES_TO_KILOBYTES(space_used));
	values[0] = storage_type;
	values[1] = space_kb;
	aez_detail_cstring_array(details, detail_count, code, 2, values);
}

static void
aez_detail_memoize_info(StringInfo details, int *detail_count,
						AezSerializeState *state,
						MemoizeState *mstate,
						List *ancestors,
						bool dynamic_only)
{
	Plan	   *plan = ((PlanState *) mstate)->plan;

	if (!dynamic_only)
	{
		ListCell   *lc;
		List	   *context;
		StringInfoData keystr;
		const char *separator = "";
		bool		useprefix;

		initStringInfo(&keystr);
		useprefix = state->rtable_size > 1 || state->verbose;
		context = set_deparse_context_plan(state->deparse_cxt, plan,
										   ancestors);
		foreach(lc, ((Memoize *) plan)->param_exprs)
		{
			Node	   *expr = (Node *) lfirst(lc);

			appendStringInfoString(&keystr, separator);
			appendStringInfoString(&keystr,
								   deparse_expression(expr, context,
													  useprefix, false));
			separator = ", ";
		}

		aez_detail_string(details, detail_count,
						  AEZ_DETAIL_MEMOIZE_CACHE_KEY,
						  keystr.data);
		aez_detail_string(details, detail_count,
						  AEZ_DETAIL_MEMOIZE_CACHE_MODE,
						  mstate->binary_mode ? "binary" : "logical");
	}

	if (!state->analyze)
		return;

	if (mstate->stats.cache_misses > 0)
	{
		const char *values[6];
		char	   *worker;
		char	   *hits;
		char	   *misses;
		char	   *evictions;
		char	   *overflows;
		char	   *mem_kb;
		uint64		mem_peak;

		if (mstate->stats.mem_peak > 0)
			mem_peak = mstate->stats.mem_peak;
		else
			mem_peak = mstate->mem_used;

		worker = psprintf("%d", -1);
		hits = psprintf(UINT64_FORMAT, mstate->stats.cache_hits);
		misses = psprintf(UINT64_FORMAT, mstate->stats.cache_misses);
		evictions = psprintf(UINT64_FORMAT, mstate->stats.cache_evictions);
		overflows = psprintf(UINT64_FORMAT, mstate->stats.cache_overflows);
		mem_kb = psprintf(UINT64_FORMAT,
						  (uint64) AEZ_BYTES_TO_KILOBYTES(mem_peak));
		values[0] = worker;
		values[1] = hits;
		values[2] = misses;
		values[3] = evictions;
		values[4] = overflows;
		values[5] = mem_kb;
		aez_detail_cstring_array(details, detail_count,
								 AEZ_DETAIL_MEMOIZE_STATS, 6, values);
	}

	if (mstate->shared_info != NULL)
	{
		for (int n = 0; n < mstate->shared_info->num_workers; n++)
		{
			MemoizeInstrumentation *si = &mstate->shared_info->sinstrument[n];
			const char *values[6];
			char	   *worker;
			char	   *hits;
			char	   *misses;
			char	   *evictions;
			char	   *overflows;
			char	   *mem_kb;

			if (si->cache_misses == 0)
				continue;

			worker = psprintf("%d", n);
			hits = psprintf(UINT64_FORMAT, si->cache_hits);
			misses = psprintf(UINT64_FORMAT, si->cache_misses);
			evictions = psprintf(UINT64_FORMAT, si->cache_evictions);
			overflows = psprintf(UINT64_FORMAT, si->cache_overflows);
			mem_kb = psprintf(UINT64_FORMAT,
							  (uint64) AEZ_BYTES_TO_KILOBYTES(si->mem_peak));
			values[0] = worker;
			values[1] = hits;
			values[2] = misses;
			values[3] = evictions;
			values[4] = overflows;
			values[5] = mem_kb;
			aez_detail_cstring_array(details, detail_count,
									 AEZ_DETAIL_MEMOIZE_STATS, 6, values);
		}
	}
}

static void
aez_detail_hashagg_info(StringInfo details, int *detail_count,
						AezSerializeState *state,
						AggState *aggstate,
						bool dynamic_only)
{
	Agg		   *agg = (Agg *) aggstate->ss.ps.plan;

	if (agg->aggstrategy != AGG_HASHED &&
		agg->aggstrategy != AGG_MIXED)
		return;

	if (!dynamic_only && aggstate->hash_planned_partitions > 0)
		aez_detail_i64(details, detail_count,
					   AEZ_DETAIL_HASHAGG_PLANNED_PARTITIONS,
					   aggstate->hash_planned_partitions);

	if (state->analyze && aggstate->hash_mem_peak > 0)
	{
		const char *values[4];
		char	   *batches;
		char	   *memory;
		char	   *disk;
		char	   *worker;

		worker = psprintf("%d", -1);
		batches = psprintf("%d", aggstate->hash_batches_used);
		memory = psprintf(INT64_FORMAT,
						  (int64) AEZ_BYTES_TO_KILOBYTES(aggstate->hash_mem_peak));
		disk = psprintf(UINT64_FORMAT, aggstate->hash_disk_used);
		values[0] = worker;
		values[1] = batches;
		values[2] = memory;
		values[3] = disk;
		aez_detail_cstring_array(details, detail_count,
								 AEZ_DETAIL_HASHAGG_STATS, 4, values);
	}

	if (state->analyze && aggstate->shared_info != NULL)
	{
		for (int n = 0; n < aggstate->shared_info->num_workers; n++)
		{
			AggregateInstrumentation *sinstrument;
			const char *values[4];
			char	   *worker;
			char	   *batches;
			char	   *memory;
			char	   *disk;

			sinstrument = &aggstate->shared_info->sinstrument[n];
			if (sinstrument->hash_mem_peak == 0)
				continue;

			worker = psprintf("%d", n);
			batches = psprintf("%d", sinstrument->hash_batches_used);
			memory = psprintf(INT64_FORMAT,
							  (int64) AEZ_BYTES_TO_KILOBYTES(sinstrument->hash_mem_peak));
			disk = psprintf(UINT64_FORMAT, sinstrument->hash_disk_used);
			values[0] = worker;
			values[1] = batches;
			values[2] = memory;
			values[3] = disk;
			aez_detail_cstring_array(details, detail_count,
									 AEZ_DETAIL_HASHAGG_STATS, 4, values);
		}
	}
}

static void
aez_detail_index_searches(StringInfo details, int *detail_count,
						  AezSerializeState *state,
						  PlanState *planstate)
{
	Plan	   *plan = planstate->plan;
	SharedIndexScanInstrumentation *sharedInfo = NULL;
	uint64		nsearches = 0;

	if (!state->analyze)
		return;

	switch (nodeTag(plan))
	{
		case T_IndexScan:
			nsearches = ((IndexScanState *) planstate)->iss_Instrument.nsearches;
			sharedInfo = ((IndexScanState *) planstate)->iss_SharedInfo;
			break;
		case T_IndexOnlyScan:
			nsearches = ((IndexOnlyScanState *) planstate)->ioss_Instrument.nsearches;
			sharedInfo = ((IndexOnlyScanState *) planstate)->ioss_SharedInfo;
			break;
		case T_BitmapIndexScan:
			nsearches = ((BitmapIndexScanState *) planstate)->biss_Instrument.nsearches;
			sharedInfo = ((BitmapIndexScanState *) planstate)->biss_SharedInfo;
			break;
		default:
			return;
	}

	if (sharedInfo)
	{
		for (int i = 0; i < sharedInfo->num_workers; ++i)
			nsearches += sharedInfo->winstrument[i].nsearches;
	}

	aez_detail_u64(details, detail_count, AEZ_DETAIL_INDEX_SEARCHES,
				   nsearches);
}

static void
aez_detail_bitmap_heap_blocks(StringInfo details, int *detail_count,
							  AezSerializeState *state,
							  BitmapHeapScanState *planstate)
{
	const char *values[3];
	char	   *worker;
	char	   *exact;
	char	   *lossy;

	if (!state->analyze)
		return;

	worker = psprintf("%d", -1);
	exact = psprintf(UINT64_FORMAT, planstate->stats.exact_pages);
	lossy = psprintf(UINT64_FORMAT, planstate->stats.lossy_pages);
	values[0] = worker;
	values[1] = exact;
	values[2] = lossy;
	aez_detail_cstring_array(details, detail_count,
							 AEZ_DETAIL_BITMAP_HEAP_BLOCKS, 3, values);

	if (planstate->pstate != NULL)
	{
		for (int n = 0; n < planstate->sinstrument->num_workers; n++)
		{
			BitmapHeapScanInstrumentation *si =
				&planstate->sinstrument->sinstrument[n];

			if (si->exact_pages == 0 && si->lossy_pages == 0)
				continue;

			worker = psprintf("%d", n);
			exact = psprintf(UINT64_FORMAT, si->exact_pages);
			lossy = psprintf(UINT64_FORMAT, si->lossy_pages);
			values[0] = worker;
			values[1] = exact;
			values[2] = lossy;
			aez_detail_cstring_array(details, detail_count,
									 AEZ_DETAIL_BITMAP_HEAP_BLOCKS, 3, values);
		}
	}
}

static void
aez_detail_modifytable_info(StringInfo details, int *detail_count,
							AezSerializeState *state,
							ModifyTableState *mtstate,
							List *ancestors,
							bool dynamic_only)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;

	if (!dynamic_only && node->onConflictAction != ONCONFLICT_NONE)
	{
		ListCell   *lc;

		aez_detail_string(details, detail_count,
						  AEZ_DETAIL_CONFLICT_RESOLUTION,
						  node->onConflictAction == ONCONFLICT_NOTHING ?
						  "NOTHING" : "UPDATE");
		foreach(lc, node->arbiterIndexes)
		{
			char	   *indexname = get_rel_name(lfirst_oid(lc));

			aez_detail_string(details, detail_count,
							  AEZ_DETAIL_CONFLICT_ARBITER_INDEX,
							  indexname);
		}
		if (node->onConflictWhere)
		{
			aez_detail_expr(details, detail_count, state,
							&mtstate->ps, ancestors,
							node->onConflictWhere,
							AEZ_DETAIL_CONFLICT_FILTER,
							state->rtable_size > 1 || state->verbose);
			aez_detail_instrumentation_count(details, detail_count,
											 state, &mtstate->ps,
											 AEZ_DETAIL_ROWS_REMOVED_BY_CONFLICT_FILTER,
											 1);
		}
	}
	else if (!dynamic_only && node->operation == CMD_MERGE)
	{
		aez_detail_merge_actions(details, detail_count, state, mtstate,
								 ancestors);
	}

	if (!state->analyze || !mtstate->ps.instrument)
		return;

	if (node->onConflictAction != ONCONFLICT_NONE && outerPlanState(mtstate))
	{
		const char *values[2];
		char	   *inserted;
		char	   *conflicting;
		double		total;
		double		other_path;

		if (outerPlanState(mtstate)->instrument)
			InstrEndLoop(outerPlanState(mtstate)->instrument);
		total = outerPlanState(mtstate)->instrument ?
			outerPlanState(mtstate)->instrument->ntuples : 0.0;
		other_path = mtstate->ps.instrument->ntuples2;
		inserted = psprintf("%.0f", total - other_path);
		conflicting = psprintf("%.0f", other_path);
		values[0] = inserted;
		values[1] = conflicting;
		aez_detail_cstring_array(details, detail_count,
								 AEZ_DETAIL_CONFLICT_TUPLES, 2, values);
	}
	else if (node->operation == CMD_MERGE && outerPlanState(mtstate))
	{
		const char *values[4];
		char	   *inserted;
		char	   *updated;
		char	   *deleted;
		char	   *skipped;
		double		total;
		double		skipped_path;

		if (outerPlanState(mtstate)->instrument)
			InstrEndLoop(outerPlanState(mtstate)->instrument);
		total = outerPlanState(mtstate)->instrument ?
			outerPlanState(mtstate)->instrument->ntuples : 0.0;
		skipped_path = total - mtstate->mt_merge_inserted -
			mtstate->mt_merge_updated - mtstate->mt_merge_deleted;
		if (skipped_path < 0)
			skipped_path = 0;

		inserted = psprintf("%.0f", mtstate->mt_merge_inserted);
		updated = psprintf("%.0f", mtstate->mt_merge_updated);
		deleted = psprintf("%.0f", mtstate->mt_merge_deleted);
		skipped = psprintf("%.0f", skipped_path);
		values[0] = inserted;
		values[1] = updated;
		values[2] = deleted;
		values[3] = skipped;
		aez_detail_cstring_array(details, detail_count,
								 AEZ_DETAIL_MERGE_TUPLES, 4, values);
	}
}

static void
aez_detail_merge_actions(StringInfo details, int *detail_count,
						 AezSerializeState *state,
						 ModifyTableState *mtstate,
						 List *ancestors)
{
	List	   *actions = NIL;
	ListCell   *lc;
	int			relno = 0;

	foreach(lc, mtstate->mt_mergeActionLists)
	{
		List	   *mergeActionList = (List *) lfirst(lc);
		ResultRelInfo *resultRelInfo;
		ListCell   *ac;

		if (relno >= mtstate->mt_nrels)
			break;
		resultRelInfo = mtstate->resultRelInfo + relno;
		relno++;

		foreach(ac, mergeActionList)
		{
			MergeAction *action = lfirst_node(MergeAction, ac);

			actions = lappend(actions,
							  aez_deparse_merge_action(state, mtstate,
													   resultRelInfo,
													   action, ancestors));
		}
	}

	aez_detail_string_list(details, detail_count, AEZ_DETAIL_MERGE_ACTION,
						   actions);
}

static char *
aez_deparse_merge_action(AezSerializeState *state,
						 ModifyTableState *mtstate,
						 ResultRelInfo *resultRelInfo,
						 MergeAction *action,
						 List *ancestors)
{
	StringInfoData buf;
	List	   *context;
	ListCell   *lc;
	const char *sep;

	initStringInfo(&buf);
	context = set_deparse_context_plan(state->deparse_cxt,
									   mtstate->ps.plan,
									   ancestors);

	appendStringInfo(&buf, "WHEN %s", aez_merge_match_name(action->matchKind));
	if (action->qual)
		appendStringInfo(&buf, " AND %s",
						 deparse_expression(action->qual, context,
											state->rtable_size > 1 ||
											state->verbose,
											false));

	appendStringInfo(&buf, " THEN %s", aez_command_name(action->commandType));

	switch (action->commandType)
	{
		case CMD_INSERT:
			if (action->targetList == NIL)
			{
				appendStringInfoString(&buf, " DEFAULT VALUES");
				break;
			}

			appendStringInfoString(&buf, " (");
			sep = "";
			foreach(lc, action->targetList)
			{
				TargetEntry *tle = lfirst_node(TargetEntry, lc);
				const char *attname = tle->resname;

				if (tle->resjunk)
					continue;
				if (resultRelInfo->ri_RelationDesc && tle->resno > 0)
					attname = get_attname(RelationGetRelid(resultRelInfo->ri_RelationDesc),
										  tle->resno, true);
				appendStringInfo(&buf, "%s%s", sep,
								 quote_identifier(attname ? attname : "?column?"));
				sep = ", ";
			}
			appendStringInfoString(&buf, ") VALUES (");
			sep = "";
			foreach(lc, action->targetList)
			{
				TargetEntry *tle = lfirst_node(TargetEntry, lc);

				if (tle->resjunk)
					continue;
				appendStringInfo(&buf, "%s%s", sep,
								 deparse_expression((Node *) tle->expr,
													context,
													state->rtable_size > 1 ||
													state->verbose,
													false));
				sep = ", ";
			}
			appendStringInfoChar(&buf, ')');
			break;

		case CMD_UPDATE:
			appendStringInfoString(&buf, " SET ");
			sep = "";
			foreach(lc, action->targetList)
			{
				TargetEntry *tle = lfirst_node(TargetEntry, lc);
				const char *attname = tle->resname;

				if (tle->resjunk)
					continue;
				if (resultRelInfo->ri_RelationDesc && tle->resno > 0)
					attname = get_attname(RelationGetRelid(resultRelInfo->ri_RelationDesc),
										  tle->resno, true);
				appendStringInfo(&buf, "%s%s = %s", sep,
								 quote_identifier(attname ? attname : "?column?"),
								 deparse_expression((Node *) tle->expr,
													context,
													state->rtable_size > 1 ||
													state->verbose,
													false));
				sep = ", ";
			}
			break;

		case CMD_DELETE:
		case CMD_NOTHING:
			break;

		default:
			appendStringInfo(&buf, " /* unknown command %d */",
							 (int) action->commandType);
			break;
	}

	return buf.data;
}

static const char *
aez_merge_match_name(MergeMatchKind matchKind)
{
	switch (matchKind)
	{
		case MERGE_WHEN_MATCHED:
			return "MATCHED";
		case MERGE_WHEN_NOT_MATCHED_BY_SOURCE:
			return "NOT MATCHED BY SOURCE";
		case MERGE_WHEN_NOT_MATCHED_BY_TARGET:
			return "NOT MATCHED BY TARGET";
	}

	return "UNKNOWN";
}

static const char *
aez_command_name(CmdType commandType)
{
	switch (commandType)
	{
		case CMD_INSERT:
			return "INSERT";
		case CMD_UPDATE:
			return "UPDATE";
		case CMD_DELETE:
			return "DELETE";
		case CMD_NOTHING:
			return "DO NOTHING";
		default:
			return "UNKNOWN";
	}
}

static void
aez_serialize_node_details(StringInfo details, int *detail_count,
						   AezSerializeState *state,
						   PlanState *planstate,
						   List *ancestors)
{
	Plan	   *plan = planstate->plan;

	if (state->verbose)
		aez_detail_targetlist(details, detail_count, state, planstate,
							  ancestors);

	switch (nodeTag(plan))
	{
		case T_IndexScan:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((IndexScan *) plan)->indexqualorig,
							AEZ_DETAIL_INDEX_COND,
							state->verbose);
			if (((IndexScan *) plan)->indexqualorig)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_INDEX_RECHECK,
												 2);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((IndexScan *) plan)->indexorderbyorig,
							AEZ_DETAIL_INDEX_ORDER_BY,
							state->verbose);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;

		case T_IndexOnlyScan:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((IndexOnlyScan *) plan)->indexqual,
							AEZ_DETAIL_INDEX_COND,
							state->verbose);
			if (((IndexOnlyScan *) plan)->recheckqual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_INDEX_RECHECK,
												 2);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((IndexOnlyScan *) plan)->indexorderby,
							AEZ_DETAIL_INDEX_ORDER_BY,
							state->verbose);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			if (state->analyze && planstate->instrument)
				aez_detail_double(details, detail_count,
								  AEZ_DETAIL_HEAP_FETCHES,
								  planstate->instrument->ntuples2);
			break;

		case T_BitmapIndexScan:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((BitmapIndexScan *) plan)->indexqualorig,
							AEZ_DETAIL_INDEX_COND,
							state->verbose);
			break;

		case T_BitmapHeapScan:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((BitmapHeapScan *) plan)->bitmapqualorig,
							AEZ_DETAIL_RECHECK_COND,
							state->verbose);
			if (((BitmapHeapScan *) plan)->bitmapqualorig)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_INDEX_RECHECK,
												 2);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;

		case T_SampleScan:
		case T_SeqScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
		case T_SubqueryScan:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							IsA(plan, SubqueryScan) || state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;

		case T_Gather:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			aez_detail_double(details, detail_count,
							  AEZ_DETAIL_WORKERS_PLANNED,
							  ((Gather *) plan)->num_workers);
			if (state->analyze)
				aez_detail_double(details, detail_count,
								  AEZ_DETAIL_WORKERS_LAUNCHED,
								  ((GatherState *) planstate)->nworkers_launched);
			break;

		case T_GatherMerge:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			aez_detail_double(details, detail_count,
							  AEZ_DETAIL_WORKERS_PLANNED,
							  ((GatherMerge *) plan)->num_workers);
			if (state->analyze)
				aez_detail_double(details, detail_count,
								  AEZ_DETAIL_WORKERS_LAUNCHED,
								  ((GatherMergeState *) planstate)->nworkers_launched);
			aez_detail_sort_group_keys(details, detail_count, state,
									   planstate, ancestors,
									   AEZ_DETAIL_SORT_KEY,
									   ((GatherMerge *) plan)->numCols, 0,
									   ((GatherMerge *) plan)->sortColIdx,
									   ((GatherMerge *) plan)->sortOperators,
									   ((GatherMerge *) plan)->collations,
									   ((GatherMerge *) plan)->nullsFirst);
			break;

		case T_FunctionScan:
			if (state->verbose)
			{
				List	   *fexprs = NIL;
				ListCell   *lc;

				foreach(lc, ((FunctionScan *) plan)->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

					fexprs = lappend(fexprs, rtfunc->funcexpr);
				}
				aez_detail_expr(details, detail_count, state, planstate,
								ancestors, (Node *) fexprs,
								AEZ_DETAIL_FUNCTION_CALL,
								state->verbose);
			}
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;

		case T_TableFuncScan:
			if (state->verbose)
				aez_detail_expr(details, detail_count, state, planstate,
								ancestors,
								(Node *) ((TableFuncScan *) plan)->tablefunc,
								AEZ_DETAIL_TABLE_FUNCTION_CALL,
								state->verbose);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;

		case T_TidScan:
			{
				List	   *tidquals = ((TidScan *) plan)->tidquals;

				if (list_length(tidquals) > 1)
					tidquals = list_make1(make_orclause(tidquals));
				aez_detail_qual(details, detail_count, state, planstate,
								ancestors, tidquals, AEZ_DETAIL_TID_COND,
								state->verbose);
				aez_detail_qual(details, detail_count, state, planstate,
								ancestors, plan->qual, AEZ_DETAIL_FILTER,
								state->verbose);
				if (plan->qual)
					aez_detail_instrumentation_count(details, detail_count,
													 state, planstate,
													 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
													 1);
			}
			break;

		case T_TidRangeScan:
			{
				List	   *tidquals = ((TidRangeScan *) plan)->tidrangequals;

				if (list_length(tidquals) > 1)
					tidquals = list_make1(make_andclause(tidquals));
				aez_detail_qual(details, detail_count, state, planstate,
								ancestors, tidquals, AEZ_DETAIL_TID_COND,
								state->verbose);
				aez_detail_qual(details, detail_count, state, planstate,
								ancestors, plan->qual, AEZ_DETAIL_FILTER,
								state->verbose);
				if (plan->qual)
					aez_detail_instrumentation_count(details, detail_count,
													 state, planstate,
													 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
													 1);
			}
			break;

		case T_ForeignScan:
		case T_CustomScan:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;

		case T_NestLoop:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((NestLoop *) plan)->join.joinqual,
							AEZ_DETAIL_JOIN_FILTER,
							state->rtable_size > 1 || state->verbose);
			if (((NestLoop *) plan)->join.joinqual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_JOIN_FILTER,
												 1);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->rtable_size > 1 || state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 2);
			break;

		case T_MergeJoin:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((MergeJoin *) plan)->mergeclauses,
							AEZ_DETAIL_MERGE_COND,
							state->rtable_size > 1 || state->verbose);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((MergeJoin *) plan)->join.joinqual,
							AEZ_DETAIL_JOIN_FILTER,
							state->rtable_size > 1 || state->verbose);
			if (((MergeJoin *) plan)->join.joinqual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_JOIN_FILTER,
												 1);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->rtable_size > 1 || state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 2);
			break;

		case T_HashJoin:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((HashJoin *) plan)->hashclauses,
							AEZ_DETAIL_HASH_COND,
							state->rtable_size > 1 || state->verbose);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((HashJoin *) plan)->join.joinqual,
							AEZ_DETAIL_JOIN_FILTER,
							state->rtable_size > 1 || state->verbose);
			if (((HashJoin *) plan)->join.joinqual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_JOIN_FILTER,
												 1);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->rtable_size > 1 || state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 2);
			break;

		case T_Agg:
			{
				Agg		   *agg = (Agg *) plan;
				AezDetailCode keycode;

				keycode = (agg->aggstrategy == AGG_HASHED ||
						   agg->aggstrategy == AGG_MIXED) ?
					AEZ_DETAIL_HASH_KEY : AEZ_DETAIL_GROUP_KEY;
				if (agg->numCols > 0 && outerPlanState(planstate))
				{
					List	   *child_ancestors;

					child_ancestors = lcons(plan, ancestors);
					aez_detail_sort_group_keys(details, detail_count, state,
											   outerPlanState(planstate),
											   child_ancestors, keycode,
											   agg->numCols, 0,
											   agg->grpColIdx,
											   NULL, NULL, NULL);
					child_ancestors = list_delete_first(child_ancestors);
				}
				aez_detail_qual(details, detail_count, state, planstate,
								ancestors, plan->qual, AEZ_DETAIL_FILTER,
								state->rtable_size > 1 || state->verbose);
				if (plan->qual)
					aez_detail_instrumentation_count(details, detail_count,
													 state, planstate,
													 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
													 1);
			}
			break;

		case T_WindowAgg:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							((WindowAgg *) plan)->runConditionOrig,
							AEZ_DETAIL_RUN_CONDITION,
							state->rtable_size > 1 || state->verbose);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->rtable_size > 1 || state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;

		case T_Group:
			if (outerPlanState(planstate))
			{
				Group	   *group = (Group *) plan;
				List	   *child_ancestors;

				child_ancestors = lcons(plan, ancestors);
				aez_detail_sort_group_keys(details, detail_count, state,
										   outerPlanState(planstate),
										   child_ancestors,
										   AEZ_DETAIL_GROUP_KEY,
										   group->numCols, 0,
										   group->grpColIdx,
										   NULL, NULL, NULL);
				child_ancestors = list_delete_first(child_ancestors);
			}
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->rtable_size > 1 || state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;

		case T_Sort:
			aez_detail_sort_group_keys(details, detail_count, state,
									   planstate, ancestors,
									   AEZ_DETAIL_SORT_KEY,
									   ((Sort *) plan)->numCols, 0,
									   ((Sort *) plan)->sortColIdx,
									   ((Sort *) plan)->sortOperators,
									   ((Sort *) plan)->collations,
									   ((Sort *) plan)->nullsFirst);
			break;

		case T_IncrementalSort:
			aez_detail_sort_group_keys(details, detail_count, state,
									   planstate, ancestors,
									   AEZ_DETAIL_SORT_KEY,
									   ((IncrementalSort *) plan)->sort.numCols,
									   ((IncrementalSort *) plan)->nPresortedCols,
									   ((IncrementalSort *) plan)->sort.sortColIdx,
									   ((IncrementalSort *) plan)->sort.sortOperators,
									   ((IncrementalSort *) plan)->sort.collations,
									   ((IncrementalSort *) plan)->sort.nullsFirst);
			break;

		case T_MergeAppend:
			aez_detail_sort_group_keys(details, detail_count, state,
									   planstate, ancestors,
									   AEZ_DETAIL_SORT_KEY,
									   ((MergeAppend *) plan)->numCols, 0,
									   ((MergeAppend *) plan)->sortColIdx,
									   ((MergeAppend *) plan)->sortOperators,
									   ((MergeAppend *) plan)->collations,
									   ((MergeAppend *) plan)->nullsFirst);
			break;

		case T_Result:
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors,
							(List *) ((Result *) plan)->resconstantqual,
							AEZ_DETAIL_ONE_TIME_FILTER,
							state->rtable_size > 1 || state->verbose);
			aez_detail_qual(details, detail_count, state, planstate,
							ancestors, plan->qual, AEZ_DETAIL_FILTER,
							state->rtable_size > 1 || state->verbose);
			if (plan->qual)
				aez_detail_instrumentation_count(details, detail_count,
												 state, planstate,
												 AEZ_DETAIL_ROWS_REMOVED_BY_FILTER,
												 1);
			break;

		default:
			break;
	}

	aez_serialize_runtime_node_details(details, detail_count, state,
									   planstate, ancestors, false);
}

static void
aez_serialize_extension_explain_text(StringInfo details, int *detail_count,
									 AezSerializeState *state,
									 PlanState *planstate,
									 List *ancestors)
{
	ExplainState *es = NULL;
	MemoryContext oldcxt;
	ErrorData  *edata = NULL;

	if (!aez_node_has_extension_explain(planstate))
		return;

	oldcxt = CurrentMemoryContext;
	PG_TRY();
	{
		es = aez_new_text_explain_state(state);

		switch (nodeTag(planstate->plan))
		{
			case T_ForeignScan:
				aez_capture_foreignscan_explain(es,
												(ForeignScanState *) planstate);
				break;
			case T_CustomScan:
				{
					CustomScanState *css = (CustomScanState *) planstate;

					if (css->methods && css->methods->ExplainCustomScan)
						css->methods->ExplainCustomScan(css, ancestors, es);
				}
				break;
			case T_ModifyTable:
				aez_capture_foreignmodify_explain(es,
												  (ModifyTableState *) planstate);
				break;
			default:
				break;
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();

	if (edata != NULL)
	{
		aez_detail_string(details, detail_count, AEZ_DETAIL_EXTENSION_TEXT,
						  psprintf("Extension EXPLAIN callback failed: %s",
								   edata->message ? edata->message : "unknown error"));
		FreeErrorData(edata);
		return;
	}

	aez_emit_explain_text_detail(details, detail_count, es);
}

static bool
aez_node_has_extension_explain(PlanState *planstate)
{
	switch (nodeTag(planstate->plan))
	{
		case T_ForeignScan:
			{
				ForeignScanState *fsstate = (ForeignScanState *) planstate;
				FdwRoutine *fdwroutine = fsstate->fdwroutine;

				if (fdwroutine == NULL)
					return false;
				if (((ForeignScan *) fsstate->ss.ps.plan)->operation != CMD_SELECT)
					return fdwroutine->ExplainDirectModify != NULL;
				return fdwroutine->ExplainForeignScan != NULL;
			}
		case T_CustomScan:
			{
				CustomScanState *css = (CustomScanState *) planstate;

				return css->methods != NULL &&
					css->methods->ExplainCustomScan != NULL;
			}
		case T_ModifyTable:
			{
				ModifyTableState *mtstate = (ModifyTableState *) planstate;

				for (int j = 0; j < mtstate->mt_nrels; j++)
				{
					ResultRelInfo *resultRelInfo = mtstate->resultRelInfo + j;
					FdwRoutine *fdwroutine = resultRelInfo->ri_FdwRoutine;

					if (!resultRelInfo->ri_usesFdwDirectModify &&
						fdwroutine != NULL &&
						fdwroutine->ExplainForeignModify != NULL)
						return true;
				}
				return false;
			}
		default:
			return false;
	}
}

static bool
aez_plan_tree_has_extension_explain(PlanState *planstate)
{
	Plan	   *plan;

	if (planstate == NULL)
		return false;
	if (aez_node_has_extension_explain(planstate))
		return true;

	plan = planstate->plan;
	if (aez_subplans_have_extension_explain(planstate->initPlan))
		return true;
	if (aez_plan_tree_has_extension_explain(outerPlanState(planstate)))
		return true;
	if (aez_plan_tree_has_extension_explain(innerPlanState(planstate)))
		return true;

	switch (nodeTag(plan))
	{
		case T_Append:
			if (aez_plan_members_have_extension_explain(
					((AppendState *) planstate)->appendplans,
					((AppendState *) planstate)->as_nplans))
				return true;
			break;
		case T_MergeAppend:
			if (aez_plan_members_have_extension_explain(
					((MergeAppendState *) planstate)->mergeplans,
					((MergeAppendState *) planstate)->ms_nplans))
				return true;
			break;
		case T_BitmapAnd:
			if (aez_plan_members_have_extension_explain(
					((BitmapAndState *) planstate)->bitmapplans,
					((BitmapAndState *) planstate)->nplans))
				return true;
			break;
		case T_BitmapOr:
			if (aez_plan_members_have_extension_explain(
					((BitmapOrState *) planstate)->bitmapplans,
					((BitmapOrState *) planstate)->nplans))
				return true;
			break;
		case T_SubqueryScan:
			if (aez_plan_tree_has_extension_explain(
					((SubqueryScanState *) planstate)->subplan))
				return true;
			break;
		case T_CustomScan:
			{
				ListCell   *lc;

				foreach(lc, ((CustomScanState *) planstate)->custom_ps)
				{
					if (aez_plan_tree_has_extension_explain(
							(PlanState *) lfirst(lc)))
						return true;
				}
			}
			break;
		default:
			break;
	}

	return aez_subplans_have_extension_explain(planstate->subPlan);
}

static bool
aez_plan_members_have_extension_explain(PlanState **planstates, int nplans)
{
	for (int i = 0; i < nplans; i++)
	{
		if (aez_plan_tree_has_extension_explain(planstates[i]))
			return true;
	}
	return false;
}

static bool
aez_subplans_have_extension_explain(List *plans)
{
	ListCell   *lc;

	foreach(lc, plans)
	{
		SubPlanState *sps = lfirst_node(SubPlanState, lc);

		if (aez_plan_tree_has_extension_explain(sps->planstate))
			return true;
	}
	return false;
}

static ExplainState *
aez_new_text_explain_state(AezSerializeState *state)
{
	ExplainState *es;

	aez_init_deparse_state(state);

	es = NewExplainState();
	es->verbose = state->verbose;
	es->analyze = state->analyze;
	es->costs = true;
	es->buffers = state->analyze && auto_explain_z_log_buffers;
	es->wal = state->analyze && auto_explain_z_log_wal;
	es->timing = auto_explain_z_log_timing;
	es->summary = false;
	es->format = EXPLAIN_FORMAT_TEXT;
	es->pstmt = state->queryDesc->plannedstmt;
	es->rtable = state->rtable;
	es->rtable_names = state->rtable_names;
	es->deparse_cxt = state->deparse_cxt;
	es->printed_subplans = NULL;
	es->hide_workers = false;
	es->rtable_size = state->rtable_size;

	return es;
}

static void
aez_capture_foreignscan_explain(ExplainState *es, ForeignScanState *fsstate)
{
	FdwRoutine *fdwroutine = fsstate->fdwroutine;

	if (fdwroutine == NULL)
		return;

	if (((ForeignScan *) fsstate->ss.ps.plan)->operation != CMD_SELECT)
	{
		if (fdwroutine->ExplainDirectModify != NULL)
			fdwroutine->ExplainDirectModify(fsstate, es);
	}
	else
	{
		if (fdwroutine->ExplainForeignScan != NULL)
			fdwroutine->ExplainForeignScan(fsstate, es);
	}
}

static void
aez_capture_foreignmodify_explain(ExplainState *es, ModifyTableState *mtstate)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;

	for (int j = 0; j < mtstate->mt_nrels; j++)
	{
		ResultRelInfo *resultRelInfo = mtstate->resultRelInfo + j;
		FdwRoutine *fdwroutine = resultRelInfo->ri_FdwRoutine;
		int			save_indent;

		if (resultRelInfo->ri_usesFdwDirectModify ||
			fdwroutine == NULL ||
			fdwroutine->ExplainForeignModify == NULL)
			continue;

		save_indent = es->indent;
		if (mtstate->mt_nrels > 1)
		{
			Relation	rel = resultRelInfo->ri_RelationDesc;

			ExplainIndentText(es);
			if (rel)
				appendStringInfo(es->str, "Foreign Modify on %s:\n",
								 quote_identifier(RelationGetRelationName(rel)));
			else
				appendStringInfo(es->str, "Foreign Modify target %d:\n",
								 j + 1);
			es->indent++;
		}

		fdwroutine->ExplainForeignModify(mtstate,
										 resultRelInfo,
										 (List *) list_nth(node->fdwPrivLists, j),
										 j,
										 es);
		es->indent = save_indent;
	}
}

static void
aez_emit_explain_text_detail(StringInfo details, int *detail_count,
							 ExplainState *es)
{
	int			len = es->str->len;
	char		save;

	while (len > 0 && es->str->data[len - 1] == '\n')
		len--;
	if (len == 0)
		return;

	save = es->str->data[len];
	es->str->data[len] = '\0';
	aez_detail_string(details, detail_count, AEZ_DETAIL_EXTENSION_TEXT,
					  es->str->data);
	es->str->data[len] = save;
}

static void
aez_detail_string(StringInfo details, int *detail_count,
				  AezDetailCode code, const char *value)
{
	if (value == NULL)
		return;

	aez_put_u16(details, (uint16) code);
	aez_put_u8(details, AEZ_DETAIL_TYPE_STRING);
	aez_put_string(details, value, -1);
	(*detail_count)++;
}

static void
aez_detail_double(StringInfo details, int *detail_count,
				  AezDetailCode code, double value)
{
	aez_put_u16(details, (uint16) code);
	aez_put_u8(details, AEZ_DETAIL_TYPE_DOUBLE);
	aez_put_double(details, value);
	(*detail_count)++;
}

static void
aez_detail_i64(StringInfo details, int *detail_count,
			   AezDetailCode code, int64 value)
{
	aez_put_u16(details, (uint16) code);
	aez_put_u8(details, AEZ_DETAIL_TYPE_INT64);
	aez_put_i64(details, value);
	(*detail_count)++;
}

static void
aez_detail_u64(StringInfo details, int *detail_count,
			   AezDetailCode code, uint64 value)
{
	aez_put_u16(details, (uint16) code);
	aez_put_u8(details, AEZ_DETAIL_TYPE_UINT64);
	aez_put_u64(details, value);
	(*detail_count)++;
}

static void
aez_detail_bool(StringInfo details, int *detail_count,
				AezDetailCode code, bool value)
{
	aez_put_u16(details, (uint16) code);
	aez_put_u8(details, AEZ_DETAIL_TYPE_BOOL);
	aez_put_u8(details, value ? 1 : 0);
	(*detail_count)++;
}

static void
aez_detail_string_list(StringInfo details, int *detail_count,
					   AezDetailCode code, List *values)
{
	ListCell   *lc;

	if (values == NIL)
		return;

	aez_put_u16(details, (uint16) code);
	aez_put_u8(details, AEZ_DETAIL_TYPE_STRING_LIST);
	aez_put_u32(details, (uint32) list_length(values));
	foreach(lc, values)
		aez_put_string(details, (const char *) lfirst(lc), -1);
	(*detail_count)++;
}

static void
aez_detail_cstring_array(StringInfo details, int *detail_count,
						 AezDetailCode code, int nvalues,
						 const char **values)
{
	aez_put_u16(details, (uint16) code);
	aez_put_u8(details, AEZ_DETAIL_TYPE_STRING_LIST);
	aez_put_u32(details, (uint32) nvalues);
	for (int i = 0; i < nvalues; i++)
		aez_put_string(details, values[i], -1);
	(*detail_count)++;
}

static void
aez_detail_qual(StringInfo details, int *detail_count,
				AezSerializeState *state,
				PlanState *planstate, List *ancestors,
				List *qual, AezDetailCode code, bool useprefix)
{
	Node	   *node;

	if (qual == NIL)
		return;

	node = (Node *) make_ands_explicit(qual);
	aez_detail_expr(details, detail_count, state, planstate, ancestors,
					node, code, useprefix);
}

static void
aez_detail_expr(StringInfo details, int *detail_count,
				AezSerializeState *state,
				PlanState *planstate, List *ancestors,
				Node *node, AezDetailCode code, bool useprefix)
{
	List	   *context;
	char	   *exprstr;

	if (node == NULL)
		return;

	context = set_deparse_context_plan(state->deparse_cxt,
									   planstate->plan,
									   ancestors);
	exprstr = deparse_expression(node, context, useprefix, false);
	aez_detail_string(details, detail_count, code, exprstr);
}

static void
aez_detail_targetlist(StringInfo details, int *detail_count,
					  AezSerializeState *state,
					  PlanState *planstate, List *ancestors)
{
	Plan	   *plan = planstate->plan;
	List	   *context;
	List	   *result = NIL;
	bool		useprefix;
	ListCell   *lc;

	if (plan->targetlist == NIL)
		return;
	if (IsA(plan, Append) || IsA(plan, MergeAppend) ||
		IsA(plan, RecursiveUnion))
		return;
	if (IsA(plan, ForeignScan) &&
		((ForeignScan *) plan)->operation != CMD_SELECT)
		return;

	context = set_deparse_context_plan(state->deparse_cxt,
									   plan,
									   ancestors);
	useprefix = state->rtable_size > 1;

	foreach(lc, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		result = lappend(result,
						 deparse_expression((Node *) tle->expr, context,
											useprefix, false));
	}

	aez_detail_string_list(details, detail_count, AEZ_DETAIL_OUTPUT, result);
}

static void
aez_detail_sort_group_keys(StringInfo details, int *detail_count,
						   AezSerializeState *state,
						   PlanState *planstate, List *ancestors,
						   AezDetailCode code,
						   int nkeys, int nPresortedKeys,
						   AttrNumber *keycols,
						   Oid *sortOperators,
						   Oid *collations,
						   bool *nullsFirst)
{
	Plan	   *plan = planstate->plan;
	List	   *context;
	List	   *result = NIL;
	List	   *resultPresorted = NIL;
	StringInfoData sortkeybuf;
	bool		useprefix;

	if (nkeys <= 0 || keycols == NULL)
		return;

	initStringInfo(&sortkeybuf);
	context = set_deparse_context_plan(state->deparse_cxt,
									   plan,
									   ancestors);
	useprefix = (state->rtable_size > 1 || state->verbose);

	for (int keyno = 0; keyno < nkeys; keyno++)
	{
		AttrNumber	keyresno = keycols[keyno];
		TargetEntry *target;
		char	   *exprstr;
		char	   *keystr;

		target = get_tle_by_resno(plan->targetlist, keyresno);
		if (!target)
			elog(ERROR, "no tlist entry for key %d", keyresno);

		exprstr = deparse_expression((Node *) target->expr, context,
									 useprefix, true);
		keystr = exprstr;

		if (sortOperators != NULL)
		{
			resetStringInfo(&sortkeybuf);
			appendStringInfoString(&sortkeybuf, exprstr);
			aez_append_sortorder_options(&sortkeybuf,
										 (Node *) target->expr,
										 sortOperators[keyno],
										 collations[keyno],
										 nullsFirst[keyno]);
			keystr = pstrdup(sortkeybuf.data);
		}

		result = lappend(result, keystr);
		if (keyno < nPresortedKeys)
			resultPresorted = lappend(resultPresorted, exprstr);
	}

	aez_detail_string_list(details, detail_count, code, result);
	if (nPresortedKeys > 0)
		aez_detail_string_list(details, detail_count,
							   AEZ_DETAIL_PRESORTED_KEY,
							   resultPresorted);
}

static void
aez_append_sortorder_options(StringInfo buf, Node *sortexpr,
							 Oid sortOperator, Oid collation,
							 bool nullsFirst)
{
	Oid			sortcoltype = exprType(sortexpr);
	bool		reverse = false;
	TypeCacheEntry *typentry;

	typentry = lookup_type_cache(sortcoltype,
								 TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

	if (OidIsValid(collation) && collation != get_typcollation(sortcoltype))
	{
		char	   *collname = get_collation_name(collation);

		if (collname == NULL)
			elog(ERROR, "cache lookup failed for collation %u", collation);
		appendStringInfo(buf, " COLLATE %s", quote_identifier(collname));
	}

	if (sortOperator == typentry->gt_opr)
	{
		appendStringInfoString(buf, " DESC");
		reverse = true;
	}
	else if (sortOperator != typentry->lt_opr)
	{
		char	   *opname = get_opname(sortOperator);

		if (opname == NULL)
			elog(ERROR, "cache lookup failed for operator %u", sortOperator);
		appendStringInfo(buf, " USING %s", opname);
		(void) get_equality_op_for_ordering_op(sortOperator, &reverse);
	}

	if (nullsFirst && !reverse)
		appendStringInfoString(buf, " NULLS FIRST");
	else if (!nullsFirst && reverse)
		appendStringInfoString(buf, " NULLS LAST");
}

static void
aez_detail_instrumentation_count(StringInfo details, int *detail_count,
								 AezSerializeState *state,
								 PlanState *planstate,
								 AezDetailCode code, int which)
{
	double		nfiltered;
	double		nloops;

	if (!state->analyze || !planstate->instrument)
		return;

	if (which == 2)
		nfiltered = planstate->instrument->nfiltered2;
	else
		nfiltered = planstate->instrument->nfiltered1;
	nloops = planstate->instrument->nloops;

	if (nloops > 0)
		aez_detail_double(details, detail_count, code, nfiltered / nloops);
	else
		aez_detail_double(details, detail_count, code, 0.0);
}

static void
aez_put_u8(StringInfo buf, uint8 value)
{
	appendBinaryStringInfo(buf, (const char *) &value, sizeof(value));
}

static void
aez_put_u16(StringInfo buf, uint16 value)
{
	uint8		bytes[2];

	bytes[0] = value & 0xff;
	bytes[1] = (value >> 8) & 0xff;
	appendBinaryStringInfo(buf, (const char *) bytes, sizeof(bytes));
}

static void
aez_put_u32(StringInfo buf, uint32 value)
{
	uint8		bytes[4];

	bytes[0] = value & 0xff;
	bytes[1] = (value >> 8) & 0xff;
	bytes[2] = (value >> 16) & 0xff;
	bytes[3] = (value >> 24) & 0xff;
	appendBinaryStringInfo(buf, (const char *) bytes, sizeof(bytes));
}

static void
aez_put_u64(StringInfo buf, uint64 value)
{
	uint8		bytes[8];

	for (int i = 0; i < 8; i++)
		bytes[i] = (value >> (8 * i)) & 0xff;
	appendBinaryStringInfo(buf, (const char *) bytes, sizeof(bytes));
}

static void
aez_put_i64(StringInfo buf, int64 value)
{
	aez_put_u64(buf, (uint64) value);
}

static void
aez_put_double(StringInfo buf, double value)
{
	union
	{
		double		d;
		uint64		u;
	}			conv;

	conv.d = value;
	aez_put_u64(buf, conv.u);
}

static uint8
aez_uint_size_code(uint64 value)
{
	if (value <= PG_UINT8_MAX)
		return AEZ_SIZE_1;
	if (value <= PG_UINT16_MAX)
		return AEZ_SIZE_2;
	if (value <= PG_UINT32_MAX)
		return AEZ_SIZE_4;
	return AEZ_SIZE_8;
}

static uint8
aez_int_size_code(int64 value)
{
	if (value >= PG_INT8_MIN && value <= PG_INT8_MAX)
		return AEZ_SIZE_1;
	if (value >= PG_INT16_MIN && value <= PG_INT16_MAX)
		return AEZ_SIZE_2;
	if (value >= PG_INT32_MIN && value <= PG_INT32_MAX)
		return AEZ_SIZE_4;
	return AEZ_SIZE_8;
}

static void
aez_put_uint_sized(StringInfo buf, uint64 value, uint8 code)
{
	switch (code)
	{
		case AEZ_SIZE_1:
			aez_put_u8(buf, (uint8) value);
			break;
		case AEZ_SIZE_2:
			aez_put_u16(buf, (uint16) value);
			break;
		case AEZ_SIZE_4:
			aez_put_u32(buf, (uint32) value);
			break;
		case AEZ_SIZE_8:
			aez_put_u64(buf, value);
			break;
	}
}

static void
aez_put_int_sized(StringInfo buf, int64 value, uint8 code)
{
	aez_put_uint_sized(buf, (uint64) value, code);
}

static void
aez_put_string(StringInfo buf, const char *value, int maxlen)
{
	uint32		len;
	uint8		len_code;

	if (value == NULL)
	{
		aez_put_u8(buf, AEZ_STRING_NULL_CTRL);
		return;
	}

	len = (uint32) strlen(value);
	if (maxlen >= 0 && len > (uint32) maxlen)
		len = (uint32) maxlen;

	len_code = aez_uint_size_code(len);
	aez_put_u8(buf, len_code);
	aez_put_uint_sized(buf, len, len_code);
	appendBinaryStringInfo(buf, value, len);
}
