// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#ifndef SHA256_H
#define SHA256_H

#include <string>

// SHA-256 (FIPS 180-4) — own implementation, no dependencies.
// Used by the `integrity` check of .mod modules (E8/neg18).

// Hexadecimal hash (64 chars) of the file; throws std::runtime_error if
// it cannot be read.
std::string sha256_file(const std::string& path);

// Hexadecimal hash (64 chars) of an in-memory buffer.
std::string sha256_hex(const std::string& data);

#endif // SHA256_H
