// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// Restored for the UC (coexisting) build.
//
// vita3k/util/src/logging.cpp includes <util/fork_build.h> and logs
// "FORK BUILD seq <n> ... | <changes>", but this header is not tracked in the
// public Vita3K-Plus repository -- it only exists in the author's local tree,
// so a clean checkout (including the v1.3 tag) cannot compile without it.
//
// seq 690 is the release the UC build is based on; 691 marks "690 + the UC
// patch set". FORK_BUILD_CHANGES is only evaluated when RELEASE_VERSION is
// false (see logging.cpp), but it still has to be a string literal so the
// ternary type-checks.

#pragma once

#define FORK_BUILD_SEQ 692
#define FORK_BUILD_CHANGES "UC coexisting (ATS off, high-accuracy off) + fix1: reap display/pipeline threads on GXM re-init (std::terminate), abort-aware display wait"
