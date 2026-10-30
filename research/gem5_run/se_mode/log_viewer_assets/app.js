// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause
"use strict";

// Log contents and refresh are server-rendered. JavaScript is optional.
const controls = document.getElementById("controls");
const content = document.getElementById("content");
const follow = document.getElementById("follow");

if (follow.checked)
    content.scrollTop = content.scrollHeight;
for (const id of ["logs", "auto", "follow"]) {
    document.getElementById(id).addEventListener("change",
                                                 () => { controls.submit(); });
}
