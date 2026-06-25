#pragma once
// Runs the M3 logic self-tests against a fake clock + recording apply.
// Returns the number of FAILED checks (0 = all pass).
int churn_selftest_run(void);
