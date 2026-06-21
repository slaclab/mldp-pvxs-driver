# SLAC google calendar readr ofr machine configurations

## instroduction
actual user download the json descirption for the event using the following curl command
curl -o spear.json  https://aosd.slac.stanford.edu/program_calendar/<experiment>/events.json?non_program_events=false&end_time=2026-12-31T23%3A00å%3A00-07%3A00&limit=1000

paramters:

- *start_time* string | (string | null)($date-time)
(query)
	
- *end_time* string | (string | null)($date-time)
(query)
	
- *limit* integer | (integer | null)

- experiment can be a parmater that can have one oro multiple value for examample lcls,facet and spear that rpoduce the output sotred into the exampel file
[facet.json](facet.json)
[lcls.json](lcls.json)
[spear.json](spear.json)