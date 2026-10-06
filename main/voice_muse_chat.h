/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Link's side of the voice session the Voice PE borrows from Muse
// (muse_chat.h). The session connects with Link's paired account.

#pragma once

// Re-read the pairing state and Noise host for the session. Call from a task
// with its stack in internal RAM, before a turn and after pairing.
void voice_hatch_refresh(void);
