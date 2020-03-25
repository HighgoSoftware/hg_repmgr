-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION repmgr" to load this file. \quit

-- extract the current schema name
-- NOTE: this assumes there will be only one schema matching 'repmgr_%';
-- user is responsible for ensuring this is the case

CREATE TEMPORARY TABLE repmgr_old_schema (schema_name TEXT);
INSERT INTO repmgr_old_schema (schema_name)
SELECT nspname AS schema_name
  FROM pg_catalog.pg_namespace
 WHERE nspname LIKE 'repmgr_%'
 LIMIT 1;

-- move old objects into new schema
DO $repmgr$
DECLARE
  old_schema TEXT;
BEGIN
  SELECT schema_name FROM repmgr_old_schema
    INTO old_schema;
  EXECUTE format('ALTER TABLE %I.repl_nodes SET SCHEMA repmgr', old_schema);
  EXECUTE format('ALTER TABLE %I.repl_events SET SCHEMA repmgr', old_schema);
  EXECUTE format('ALTER TABLE %I.repl_monitor SET SCHEMA repmgr', old_schema);
  EXECUTE format('DROP VIEW IF EXISTS %I.repl_show_nodes', old_schema);
  EXECUTE format('DROP VIEW IF EXISTS %I.repl_status', old_schema);
END$repmgr$;

-- convert "repmgr_$cluster.repl_nodes" to "repmgr.nodes"
CREATE TABLE repmgr.nodes (
  node_id          INTEGER     PRIMARY KEY,
  upstream_node_id INTEGER     NULL REFERENCES repmgr.nodes (node_id) DEFERRABLE,
  active           BOOLEAN     NOT NULL DEFAULT TRUE,
  node_name        TEXT        NOT NULL,
  type             TEXT        NOT NULL CHECK (type IN('primary','standby','witness','bdr')),
  location         TEXT        NOT NULL DEFAULT 'default',
  priority         INT         NOT NULL DEFAULT 100,
  conninfo         TEXT        NOT NULL,
  repluser         VARCHAR(63) NOT NULL,
  slot_name        TEXT        NULL,
  config_file      TEXT        NOT NULL
);

INSERT INTO repmgr.nodes
  (node_id, upstream_node_id, active, node_name, type, location, priority, conninfo, repluser, slot_name, config_file)
SELECT id, upstream_node_id, active, name,
       CASE WHEN type = 'master' THEN 'primary' ELSE type END,
       'default', priority, conninfo, 'unknown', slot_name, 'unknown'
  FROM repmgr.repl_nodes
 ORDER BY id;


-- convert "repmgr_$cluster.repl_event" to "event"

ALTER TABLE repmgr.repl_events RENAME TO events;

-- create new table "repmgr.voting_term"
CREATE TABLE repmgr.voting_term (
  term INT NOT NULL
);

CREATE UNIQUE INDEX voting_term_restrict
ON repmgr.voting_term ((TRUE));

CREATE RULE voting_term_delete AS
   ON DELETE TO repmgr.voting_term
   DO INSTEAD NOTHING;

INSERT INTO repmgr.voting_term (term) VALUES (1);


-- convert "repmgr_$cluster.repl_monitor" to "monitoring_history"


DO $repmgr$
DECLARE
  DECLARE server_version_num INT;
BEGIN
  SELECT setting
    FROM pg_catalog.pg_settings
   WHERE name = 'server_version_num'
    INTO server_version_num;
  IF server_version_num >= 90400 THEN
    EXECUTE $repmgr_func$
CREATE TABLE repmgr.monitoring_history (
  primary_node_id                INTEGER NOT NULL,
  standby_node_id                INTEGER NOT NULL,
  last_monitor_time              TIMESTAMP WITH TIME ZONE NOT NULL,
  last_apply_time                TIMESTAMP WITH TIME ZONE,
  last_wal_primary_location      PG_LSN NOT NULL,
  last_wal_standby_location      PG_LSN,
  replication_lag                BIGINT NOT NULL,
  apply_lag                      BIGINT NOT NULL
)
    $repmgr_func$;
    INSERT INTO repmgr.monitoring_history
      (primary_node_id, standby_node_id, last_monitor_time,  last_apply_time, last_wal_primary_location, last_wal_standby_location, replication_lag, apply_lag)
    SELECT primary_node, standby_node, last_monitor_time,  last_apply_time, last_wal_primary_location::pg_lsn, last_wal_standby_location::pg_lsn, replication_lag, apply_lag
      FROM repmgr.repl_monitor;
  ELSE
    EXECUTE $repmgr_func$
CREATE TABLE repmgr.monitoring_history (
  primary_node_id                INTEGER NOT NULL,
  standby_node_id                INTEGER NOT NULL,
  last_monitor_time              TIMESTAMP WITH TIME ZONE NOT NULL,
  last_apply_time                TIMESTAMP WITH TIME ZONE,
  last_wal_primary_location      TEXT NOT NULL,
  last_wal_standby_location      TEXT,
  replication_lag                BIGINT NOT NULL,
  apply_lag                      BIGINT NOT NULL
)
    $repmgr_func$;
    INSERT INTO repmgr.monitoring_history
      (primary_node_id, standby_node_id, last_monitor_time,  last_apply_time, last_wal_primary_location, last_wal_standby_location, replication_lag, apply_lag)
    SELECT primary_node, standby_node, last_monitor_time,  last_apply_time, last_wal_primary_location, last_wal_standby_location, replication_lag, apply_lag
      FROM repmgr.repl_monitor;

  END IF;
END$repmgr$;

CREATE INDEX idx_monitoring_history_time
          ON repmgr.monitoring_history (last_monitor_time, standby_node_id);

CREATE VIEW repmgr.show_nodes AS
   SELECT n.node_id,
          n.node_name,
          n.active,
          n.upstream_node_id,
          un.node_name AS upstream_node_name,
          n.type,
          n.priority,
          n.conninfo
     FROM repmgr.nodes n
LEFT JOIN repmgr.nodes un
       ON un.node_id = n.upstream_node_id;


/* ================= */
/* repmgrd functions */
/* ================= */

/* monitoring functions */

CREATE FUNCTION set_local_node_id(INT)
  RETURNS VOID
  AS 'MODULE_PATHNAME', 'set_local_node_id'
  LANGUAGE C STRICT;

CREATE FUNCTION get_local_node_id()
  RETURNS INT
  AS 'MODULE_PATHNAME', 'get_local_node_id'
  LANGUAGE C STRICT;

CREATE FUNCTION standby_set_last_updated()
  RETURNS TIMESTAMP WITH TIME ZONE
  AS '$libdir/repmgr', 'standby_set_last_updated'
  LANGUAGE C STRICT;

CREATE FUNCTION standby_get_last_updated()
  RETURNS TIMESTAMP WITH TIME ZONE
  AS '$libdir/repmgr', 'standby_get_last_updated'
  LANGUAGE C STRICT;


/* failover functions */

CREATE FUNCTION notify_follow_primary(INT)
  RETURNS VOID
  AS '$libdir/repmgr', 'notify_follow_primary'
  LANGUAGE C STRICT;

CREATE FUNCTION get_new_primary()
  RETURNS INT
  AS '$libdir/repmgr', 'get_new_primary'
  LANGUAGE C STRICT;

CREATE FUNCTION reset_voting_status()
  RETURNS VOID
  AS '$libdir/repmgr', 'reset_voting_status'
  LANGUAGE C STRICT;

CREATE FUNCTION am_bdr_failover_handler(INT)
  RETURNS BOOL
  AS '$libdir/repmgr', 'am_bdr_failover_handler'
  LANGUAGE C STRICT;

CREATE FUNCTION unset_bdr_failover_handler()
  RETURNS VOID
  AS '$libdir/repmgr', 'unset_bdr_failover_handler'
  LANGUAGE C STRICT;



CREATE VIEW repmgr.replication_status AS
  SELECT m.primary_node_id, m.standby_node_id, n.node_name AS standby_name,
 	     n.type AS node_type, n.active, last_monitor_time,
         CASE WHEN n.type='standby' THEN m.last_wal_primary_location ELSE NULL END AS last_wal_primary_location,
         m.last_wal_standby_location,
         CASE WHEN n.type='standby' THEN pg_catalog.pg_size_pretty(m.replication_lag) ELSE NULL END AS replication_lag,
         CASE WHEN n.type='standby' THEN
           CASE WHEN replication_lag > 0 THEN age(now(), m.last_apply_time) ELSE '0'::INTERVAL END
           ELSE NULL
         END AS replication_time_lag,
         CASE WHEN n.type='standby' THEN pg_catalog.pg_size_pretty(m.apply_lag) ELSE NULL END AS apply_lag,
         AGE(NOW(), CASE WHEN pg_catalog.pg_is_in_recovery() THEN repmgr.standby_get_last_updated() ELSE m.last_monitor_time END) AS communication_time_lag
    FROM repmgr.monitoring_history m
    JOIN repmgr.nodes n ON m.standby_node_id = n.node_id
   WHERE (m.standby_node_id, m.last_monitor_time) IN (
	          SELECT m1.standby_node_id, MAX(m1.last_monitor_time)
			    FROM repmgr.monitoring_history m1 GROUP BY 1
         );


/* drop old tables */
DROP TABLE repmgr.repl_nodes;
DROP TABLE repmgr.repl_monitor;

-- remove temporary table
DROP TABLE repmgr_old_schema;
