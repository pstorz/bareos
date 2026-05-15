In case you want check that the number of active clients don’t exceed a specific number, you can define this number here and check with the :bcommand:`status subscriptions` command.

If the Director can load a valid signed subscription contract through
:config:option:`dir/director/SubscriptionFile` or the packaged default location,
the contract’s ``backup_units`` value becomes authoritative and this directive
is only used as fallback.

However, this is only intended to give a hint. No active limiting is implemented.
