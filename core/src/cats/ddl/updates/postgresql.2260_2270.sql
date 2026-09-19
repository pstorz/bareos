-- update db schema from 2260 to 2270
-- start transaction
begin;

-- Add indexes on Job/JobHisto(ClientId, StartTime) to speed up the
-- "last matching job for this client" hot path used e.g. by accurate
-- backups and by GetFileRecord()/AccurateGetJobids() in sql_get.cc,
-- which filter on ClientId and order/limit by StartTime. Without this
-- index, these queries degrade to a sequential scan + sort as the
-- Job/JobHisto tables grow (confirmed via EXPLAIN ANALYZE against a
-- catalog seeded with ~500000 Job rows, see
-- systemtests/tests/catalog-scale).
CREATE INDEX job_clientid_starttime_idx ON Job (ClientId, StartTime);
CREATE INDEX jobhisto_clientid_starttime_idx ON JobHisto (ClientId, StartTime);

-- update the schema version
UPDATE Version SET VersionId = 2270;

commit;
set client_min_messages = warning;
analyze;
