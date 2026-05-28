/*-------------------------------------------------------------------------
 *
 * auto_explain_z_testscan.c
 *	  Minimal Custom Scan provider used by auto_explain_z TAP tests.
 *
 * This is intentionally not a SQL extension.  Tests load it through
 * session_preload_libraries so auto_explain_z can exercise Custom Scan
 * serialization and extension EXPLAIN text capture.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/explain.h"
#ifdef AEZ_HAVE_EXPLAIN_SPLIT_HEADERS
#include "commands/explain_format.h"
#endif
#include "executor/executor.h"
#include "fmgr.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

void		_PG_init(void);
void		_PG_fini(void);

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

static void aez_testscan_set_rel_pathlist(PlannerInfo *root,
										  RelOptInfo *rel,
										  Index rti,
										  RangeTblEntry *rte);
static Plan *aez_testscan_plan_custom_path(PlannerInfo *root,
										   RelOptInfo *rel,
										   CustomPath *best_path,
										   List *tlist,
										   List *clauses,
										   List *custom_plans);
static Node *aez_testscan_create_custom_scan_state(CustomScan *cscan);
static void aez_testscan_begin(CustomScanState *node, EState *estate,
							   int eflags);
static TupleTableSlot *aez_testscan_exec(CustomScanState *node);
static void aez_testscan_end(CustomScanState *node);
static void aez_testscan_rescan(CustomScanState *node);
static void aez_testscan_explain(CustomScanState *node, List *ancestors,
								 ExplainState *es);

static CustomPathMethods aez_testscan_path_methods = {
	.CustomName = "AutoExplainZTestScan",
	.PlanCustomPath = aez_testscan_plan_custom_path,
	.ReparameterizeCustomPathByChild = NULL,
};

static CustomScanMethods aez_testscan_scan_methods = {
	.CustomName = "AutoExplainZTestScan",
	.CreateCustomScanState = aez_testscan_create_custom_scan_state,
};

static CustomExecMethods aez_testscan_exec_methods = {
	.CustomName = "AutoExplainZTestScan",
	.BeginCustomScan = aez_testscan_begin,
	.ExecCustomScan = aez_testscan_exec,
	.EndCustomScan = aez_testscan_end,
	.ReScanCustomScan = aez_testscan_rescan,
	.MarkPosCustomScan = NULL,
	.RestrPosCustomScan = NULL,
	.EstimateDSMCustomScan = NULL,
	.InitializeDSMCustomScan = NULL,
	.ReInitializeDSMCustomScan = NULL,
	.InitializeWorkerCustomScan = NULL,
	.ShutdownCustomScan = NULL,
	.ExplainCustomScan = aez_testscan_explain,
};

void
_PG_init(void)
{
	RegisterCustomScanMethods(&aez_testscan_scan_methods);

	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = aez_testscan_set_rel_pathlist;
}

void
_PG_fini(void)
{
	if (set_rel_pathlist_hook == aez_testscan_set_rel_pathlist)
		set_rel_pathlist_hook = prev_set_rel_pathlist_hook;
}

static void
aez_testscan_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
							  Index rti, RangeTblEntry *rte)
{
	CustomPath *path;
	char	   *relname;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (rte->rtekind != RTE_RELATION)
		return;

	relname = get_rel_name(rte->relid);
	if (relname == NULL || strcmp(relname, "aez_customscan") != 0)
		return;

	path = makeNode(CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parent = rel;
	path->path.pathtarget = rel->reltarget;
	path->path.param_info = NULL;
	path->path.parallel_aware = false;
	path->path.parallel_safe = rel->consider_parallel;
	path->path.parallel_workers = 0;
	path->path.rows = 0;
#if PG_VERSION_NUM >= 180000
	path->path.disabled_nodes = 0;
#endif
	path->path.startup_cost = 0;
	path->path.total_cost = 0;
	path->path.pathkeys = NIL;
	path->flags = 0;
	path->custom_paths = NIL;
#if PG_VERSION_NUM >= 170000
	path->custom_restrictinfo = NIL;
#endif
	path->custom_private = NIL;
	path->methods = &aez_testscan_path_methods;

	add_path(rel, (Path *) path);
}

static Plan *
aez_testscan_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
							  CustomPath *best_path, List *tlist,
							  List *clauses, List *custom_plans)
{
	CustomScan *scan = makeNode(CustomScan);

	scan->scan.plan.targetlist = tlist;
	scan->scan.plan.qual = clauses;
	scan->scan.scanrelid = rel->relid;
	scan->flags = best_path->flags;
	scan->custom_plans = custom_plans;
	scan->custom_exprs = NIL;
	scan->custom_private = NIL;
	scan->custom_scan_tlist = NIL;
	scan->methods = &aez_testscan_scan_methods;

	return &scan->scan.plan;
}

static Node *
aez_testscan_create_custom_scan_state(CustomScan *cscan)
{
	CustomScanState *css = makeNode(CustomScanState);

	css->methods = &aez_testscan_exec_methods;
	return (Node *) css;
}

static void
aez_testscan_begin(CustomScanState *node, EState *estate, int eflags)
{
}

static TupleTableSlot *
aez_testscan_exec(CustomScanState *node)
{
	return ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

static void
aez_testscan_end(CustomScanState *node)
{
}

static void
aez_testscan_rescan(CustomScanState *node)
{
}

static void
aez_testscan_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ExplainPropertyText("auto_explain_z test custom scan", "covered", es);
}
