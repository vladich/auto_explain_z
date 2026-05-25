/* auto_explain_z is implemented by a loadable module. */
LOAD 'MODULE_PATHNAME';

CREATE FUNCTION auto_explain_z_rotate_logfile()
RETURNS boolean
AS 'MODULE_PATHNAME', 'auto_explain_z_rotate_logfile'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

REVOKE EXECUTE ON FUNCTION auto_explain_z_rotate_logfile() FROM public;
