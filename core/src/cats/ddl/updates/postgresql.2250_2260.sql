-- update db schema from 2250 to 2260
-- start transaction
begin;

-- Add MsgType (message severity, e.g. M_INFO/M_WARNING/M_ERROR/M_FATAL/...
-- as defined in core/src/lib/message_severity.h) to the Log table, so log
-- entries can be filtered/queried by severity directly in SQL instead of
-- having to parse LogText. Existing rows have no recorded severity and
-- stay NULL.
ALTER TABLE Log ADD COLUMN MsgType INTEGER;
CREATE INDEX log_msgtype_idx ON Log (MsgType);

-- update the schema version
UPDATE Version SET VersionId = 2260;

commit;
set client_min_messages = warning;
analyze;
