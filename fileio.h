// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#ifndef FILEIO_H
#define FILEIO_H

#include <string>

// C stdio-style file builtins — see fileio.cpp.
//
//   register_fileio_builtins()  idempotent; BEFORE compile (lookup).
//   fileio_constants()          creates NULL/SEEK_SET/SEEK_CUR/SEEK_END at
//                               the top of the symbols — call AFTER
//                               sym_clear() (otherwise they would be erased).
//   shutdown_fileio()           closes whatever the script left open (end of
//                               the script and REPL exit).
void register_fileio_builtins();
void fileio_constants();
void shutdown_fileio();

// Sandbox (E8/neg18): true if `name` is a file builtin (central gate
// in OP_CALL_BUILTIN when g_allow_fileio == false).
bool fileio_is_restricted(const std::string& name);

#endif
