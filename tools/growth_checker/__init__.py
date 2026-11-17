"""Growth checker: a development tool, separate from NeuralAlias.

It reads saved evidence (run logs, test and replay results) and says, case by
case, whether a build grew, held or receded. It is never loaded by a game and
is not part of the installer. Standard library only; no network, no model call.
See schema.md for the records and MASTER-PLAN.md section 0 for the method.
"""
