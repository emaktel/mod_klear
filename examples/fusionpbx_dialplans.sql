-- mod_klear demo dialplans for FusionPBX. Global star codes *9200-*9204.
-- Safe to re-run: DELETE-then-INSERT pattern makes it idempotent.
BEGIN;

DELETE FROM v_dialplans
 WHERE dialplan_number IN ('*9200','*9201','*9202','*9203','*9204')
   AND dialplan_context = 'global';

INSERT INTO v_dialplans (dialplan_uuid, domain_uuid, app_uuid, dialplan_name,
                         dialplan_number, dialplan_context, dialplan_order,
                         dialplan_enabled, dialplan_continue, dialplan_description,
                         dialplan_xml, insert_date)
VALUES
(
  gen_random_uuid(), NULL, '1f894dfb-0567-4e20-9026-d538bbaa5261',
  'klear-control', '*9200', 'global', 355, 'true', 'false',
  'mod_klear demo: baseline (klear off, raw echo)',
$xml$<extension name="klear-control" number="*9200" context="global" continue="false" global="true" order="355">
	<condition field="destination_number" expression="^\*9200$">
		<action application="answer"/>
		<action application="delay_echo" data="5000"/>
	</condition>
</extension>$xml$,
  NOW()
),
(
  gen_random_uuid(), NULL, '1f894dfb-0567-4e20-9026-d538bbaa5261',
  'klear-agent', '*9201', 'global', 356, 'true', 'false',
  'mod_klear demo: agent preset (DF neural NS, best speech preservation)',
$xml$<extension name="klear-agent" number="*9201" context="global" continue="false" global="true" order="356">
	<condition field="destination_number" expression="^\*9201$">
		<action application="answer"/>
		<action application="set" data="klear_preset=agent"/>
		<action application="klear" data="start"/>
		<action application="delay_echo" data="5000"/>
	</condition>
</extension>$xml$,
  NOW()
),
(
  gen_random_uuid(), NULL, '1f894dfb-0567-4e20-9026-d538bbaa5261',
  'klear-telephony', '*9202', 'global', 357, 'true', 'false',
  'mod_klear demo: telephony preset (AEC3 + DF aggressive)',
$xml$<extension name="klear-telephony" number="*9202" context="global" continue="false" global="true" order="357">
	<condition field="destination_number" expression="^\*9202$">
		<action application="answer"/>
		<action application="set" data="klear_preset=telephony"/>
		<action application="klear" data="start"/>
		<action application="delay_echo" data="5000"/>
	</condition>
</extension>$xml$,
  NOW()
),
(
  gen_random_uuid(), NULL, '1f894dfb-0567-4e20-9026-d538bbaa5261',
  'klear-aec-only', '*9203', 'global', 358, 'true', 'false',
  'mod_klear demo: aec_only preset (raw WebRTC AEC3, no NS)',
$xml$<extension name="klear-aec-only" number="*9203" context="global" continue="false" global="true" order="358">
	<condition field="destination_number" expression="^\*9203$">
		<action application="answer"/>
		<action application="set" data="klear_preset=aec_only"/>
		<action application="klear" data="start"/>
		<action application="delay_echo" data="5000"/>
	</condition>
</extension>$xml$,
  NOW()
),
(
  gen_random_uuid(), NULL, '1f894dfb-0567-4e20-9026-d538bbaa5261',
  'klear-hot-toggle', '*9204', 'global', 359, 'true', 'false',
  'mod_klear demo: hot toggle — preset=telephony, scheduled set verbs every 5s',
$xml$<extension name="klear-hot-toggle" number="*9204" context="global" continue="false" global="true" order="359">
	<condition field="destination_number" expression="^\*9204$">
		<action application="lua" data="klear_hot_toggle.lua"/>
	</condition>
</extension>$xml$,
  NOW()
);

COMMIT;

SELECT dialplan_number, dialplan_name, dialplan_context, dialplan_order,
       dialplan_enabled
  FROM v_dialplans
 WHERE dialplan_number LIKE '*920%'
 ORDER BY dialplan_number;
