<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Simulator dependencies

The simulator uses these upstream projects at build and runtime:

- [LVGL 9.5.0](https://github.com/lvgl/lvgl/releases/tag/v9.5.0), licensed
  under the MIT License.
- [SDL 2.32.10](https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10),
  licensed under the zlib License.

CMake prefers a compatible system SDL2 package. It fetches the pinned LVGL
archive by default so the simulator always uses its required fonts, drivers,
and private APIs, and fetches SDL when no compatible package is installed.
Their source is not vendored in this repository.
