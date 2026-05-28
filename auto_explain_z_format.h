#ifndef AUTO_EXPLAIN_Z_FORMAT_H
#define AUTO_EXPLAIN_Z_FORMAT_H

typedef enum AezPlanNodeCode
{
	AEZ_PLAN_NODE_UNKNOWN = 0,
	AEZ_PLAN_NODE_RESULT = 1,
	AEZ_PLAN_NODE_PROJECT_SET = 2,
	AEZ_PLAN_NODE_MODIFY_TABLE = 3,
	AEZ_PLAN_NODE_APPEND = 4,
	AEZ_PLAN_NODE_MERGE_APPEND = 5,
	AEZ_PLAN_NODE_RECURSIVE_UNION = 6,
	AEZ_PLAN_NODE_BITMAP_AND = 7,
	AEZ_PLAN_NODE_BITMAP_OR = 8,
	AEZ_PLAN_NODE_SEQ_SCAN = 9,
	AEZ_PLAN_NODE_SAMPLE_SCAN = 10,
	AEZ_PLAN_NODE_INDEX_SCAN = 11,
	AEZ_PLAN_NODE_INDEX_ONLY_SCAN = 12,
	AEZ_PLAN_NODE_BITMAP_INDEX_SCAN = 13,
	AEZ_PLAN_NODE_BITMAP_HEAP_SCAN = 14,
	AEZ_PLAN_NODE_TID_SCAN = 15,
	AEZ_PLAN_NODE_TID_RANGE_SCAN = 16,
	AEZ_PLAN_NODE_SUBQUERY_SCAN = 17,
	AEZ_PLAN_NODE_FUNCTION_SCAN = 18,
	AEZ_PLAN_NODE_VALUES_SCAN = 19,
	AEZ_PLAN_NODE_TABLE_FUNCTION_SCAN = 20,
	AEZ_PLAN_NODE_CTE_SCAN = 21,
	AEZ_PLAN_NODE_NAMED_TUPLESTORE_SCAN = 22,
	AEZ_PLAN_NODE_WORKTABLE_SCAN = 23,
	AEZ_PLAN_NODE_FOREIGN_SCAN = 24,
	AEZ_PLAN_NODE_CUSTOM_SCAN = 25,
	AEZ_PLAN_NODE_NESTED_LOOP = 26,
	AEZ_PLAN_NODE_MERGE_JOIN = 27,
	AEZ_PLAN_NODE_HASH_JOIN = 28,
	AEZ_PLAN_NODE_MATERIALIZE = 29,
	AEZ_PLAN_NODE_MEMOIZE = 30,
	AEZ_PLAN_NODE_SORT = 31,
	AEZ_PLAN_NODE_INCREMENTAL_SORT = 32,
	AEZ_PLAN_NODE_GROUP = 33,
	AEZ_PLAN_NODE_AGGREGATE = 34,
	AEZ_PLAN_NODE_WINDOW_AGG = 35,
	AEZ_PLAN_NODE_UNIQUE = 36,
	AEZ_PLAN_NODE_GATHER = 37,
	AEZ_PLAN_NODE_GATHER_MERGE = 38,
	AEZ_PLAN_NODE_HASH = 39,
	AEZ_PLAN_NODE_SETOP = 40,
	AEZ_PLAN_NODE_LOCK_ROWS = 41,
	AEZ_PLAN_NODE_LIMIT = 42
} AezPlanNodeCode;

static inline const char *
aez_plan_node_code_name(unsigned code)
{
	switch (code)
	{
		case AEZ_PLAN_NODE_RESULT:
			return "Result";
		case AEZ_PLAN_NODE_PROJECT_SET:
			return "ProjectSet";
		case AEZ_PLAN_NODE_MODIFY_TABLE:
			return "ModifyTable";
		case AEZ_PLAN_NODE_APPEND:
			return "Append";
		case AEZ_PLAN_NODE_MERGE_APPEND:
			return "Merge Append";
		case AEZ_PLAN_NODE_RECURSIVE_UNION:
			return "Recursive Union";
		case AEZ_PLAN_NODE_BITMAP_AND:
			return "BitmapAnd";
		case AEZ_PLAN_NODE_BITMAP_OR:
			return "BitmapOr";
		case AEZ_PLAN_NODE_SEQ_SCAN:
			return "Seq Scan";
		case AEZ_PLAN_NODE_SAMPLE_SCAN:
			return "Sample Scan";
		case AEZ_PLAN_NODE_INDEX_SCAN:
			return "Index Scan";
		case AEZ_PLAN_NODE_INDEX_ONLY_SCAN:
			return "Index Only Scan";
		case AEZ_PLAN_NODE_BITMAP_INDEX_SCAN:
			return "Bitmap Index Scan";
		case AEZ_PLAN_NODE_BITMAP_HEAP_SCAN:
			return "Bitmap Heap Scan";
		case AEZ_PLAN_NODE_TID_SCAN:
			return "Tid Scan";
		case AEZ_PLAN_NODE_TID_RANGE_SCAN:
			return "Tid Range Scan";
		case AEZ_PLAN_NODE_SUBQUERY_SCAN:
			return "Subquery Scan";
		case AEZ_PLAN_NODE_FUNCTION_SCAN:
			return "Function Scan";
		case AEZ_PLAN_NODE_VALUES_SCAN:
			return "Values Scan";
		case AEZ_PLAN_NODE_TABLE_FUNCTION_SCAN:
			return "Table Function Scan";
		case AEZ_PLAN_NODE_CTE_SCAN:
			return "CTE Scan";
		case AEZ_PLAN_NODE_NAMED_TUPLESTORE_SCAN:
			return "Named Tuplestore Scan";
		case AEZ_PLAN_NODE_WORKTABLE_SCAN:
			return "WorkTable Scan";
		case AEZ_PLAN_NODE_FOREIGN_SCAN:
			return "Foreign Scan";
		case AEZ_PLAN_NODE_CUSTOM_SCAN:
			return "Custom Scan";
		case AEZ_PLAN_NODE_NESTED_LOOP:
			return "Nested Loop";
		case AEZ_PLAN_NODE_MERGE_JOIN:
			return "Merge Join";
		case AEZ_PLAN_NODE_HASH_JOIN:
			return "Hash Join";
		case AEZ_PLAN_NODE_MATERIALIZE:
			return "Materialize";
		case AEZ_PLAN_NODE_MEMOIZE:
			return "Memoize";
		case AEZ_PLAN_NODE_SORT:
			return "Sort";
		case AEZ_PLAN_NODE_INCREMENTAL_SORT:
			return "Incremental Sort";
		case AEZ_PLAN_NODE_GROUP:
			return "Group";
		case AEZ_PLAN_NODE_AGGREGATE:
			return "Aggregate";
		case AEZ_PLAN_NODE_WINDOW_AGG:
			return "WindowAgg";
		case AEZ_PLAN_NODE_UNIQUE:
			return "Unique";
		case AEZ_PLAN_NODE_GATHER:
			return "Gather";
		case AEZ_PLAN_NODE_GATHER_MERGE:
			return "Gather Merge";
		case AEZ_PLAN_NODE_HASH:
			return "Hash";
		case AEZ_PLAN_NODE_SETOP:
			return "SetOp";
		case AEZ_PLAN_NODE_LOCK_ROWS:
			return "LockRows";
		case AEZ_PLAN_NODE_LIMIT:
			return "Limit";
		default:
			return NULL;
	}
}

#endif
