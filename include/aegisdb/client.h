/* Built-in client + token tooling, exposed as `aegisdb` subcommands so the
 * single binary can both run the server and talk to one (no nc/JSON by hand).
 *
 *   aegisdb client [--host H] [--port P] [--token T] <op> [args]
 *   aegisdb gen-token [--namespace NS] [--scope ro|rw|admin] [--token SECRET]
 *
 * Host/port/token also read $AEGIS_HOST / $AEGIS_PORT / $AEGIS_TOKEN. */
#ifndef AEGISDB_CLIENT_H
#define AEGISDB_CLIENT_H

/* Run the `client` subcommand. argv[0] is "client". Returns a process exit code
 * (0 = ok response, 1 = error/!ok, 2 = usage error). */
int client_main(int argc, char **argv);

/* Run the `gen-token` subcommand. argv[0] is "gen-token". Prints a token-file
 * line and the plaintext token. Returns a process exit code. */
int gen_token_main(int argc, char **argv);

/* Run the `gen-key` subcommand. argv[0] is "gen-key". Prints a fresh random
 * 32-byte encryption key as 64 hex chars for --encryption-key-file. */
int gen_key_main(int argc, char **argv);

#endif /* AEGISDB_CLIENT_H */