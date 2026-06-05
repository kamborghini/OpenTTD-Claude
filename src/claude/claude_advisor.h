/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file claude_advisor.h Public interface to the in-game Claude AI strategy advisor. */

#ifndef CLAUDE_ADVISOR_H
#define CLAUDE_ADVISOR_H

#include <string>

/**
 * Open (or focus) the Claude advisor window. When @p question is not empty it is
 * immediately submitted to Claude and the answer appears in the window.
 * @param question Optional question to ask straight away.
 */
void ShowClaudeAdvisorWindow(const std::string &question = "");

#endif /* CLAUDE_ADVISOR_H */
