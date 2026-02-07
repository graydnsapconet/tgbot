#pragma once

/**
 * Parse CLI arguments and dispatch subcommands.
 * Returns:
 *   0  - a subcommand was handled; the process should exit with the returned
 *         value stored in *exit_code.
 *   1  - no subcommand matched; the caller should proceed with daemon startup.
 */
int cli_dispatch(int argc, char **argv, int *exit_code);
