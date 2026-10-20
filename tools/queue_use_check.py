#!/usr/bin/env python3
"""Count uses of the GPU's queues past VkContext.

vkQueueSubmit, vkQueuePresentKHR, vkQueueWaitIdle and vkDeviceWaitIdle all need
the queue externally synchronized: two threads calling them at once on one
queue is undefined, and on these drivers it loses the device. The graphics
queue is shared by the frame and every upload,
and the frame is moving to a thread of its own. So every one of them goes
through VkContext, which takes the queue lock (lockQueues, waitIdle), and
ImGui's draw data is extracted before recording; its Vulkan backend must never
record a live context or issue hidden texture uploads from a recording path.

    tools/queue_use_check.py

Prints "<N> uses of the GPU queues past VkContext", then each one.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCANNED = ("src", "tools")
SUFFIXES = (".cpp", ".hpp", ".h", ".cc", ".inl")
QUEUE_CALL = re.compile(r"\b(vkQueueSubmit2?|vkQueuePresentKHR|vkQueueWaitIdle|vkDeviceWaitIdle)\s*\(")
IMGUI_RECORD = re.compile(r"\bImGui_ImplVulkan_RenderDrawData\s*\(")
# Where each may be called.
QUEUE_HOME = "src/rendering/vk_context.cpp"
LINE_COMMENT = re.compile(r"//.*$")


def main() -> int:
    found = []
    inside = 0
    for top in SCANNED:
        for path in sorted((ROOT / top).rglob("*")):
            if path.suffix not in SUFFIXES:
                continue
            rel = path.relative_to(ROOT).as_posix()
            text = path.read_text(encoding="utf-8", errors="replace")
            for number, line in enumerate(text.splitlines(), 1):
                code = LINE_COMMENT.sub("", line)
                if QUEUE_CALL.search(code):
                    if rel == QUEUE_HOME:
                        inside += 1
                    else:
                        found.append(f"{rel}:{number}: {line.strip()}")
                if IMGUI_RECORD.search(code):
                    found.append(f"{rel}:{number}: {line.strip()}")
    print(f"{len(found)} uses of the GPU queues past VkContext")
    print(f"{inside} inside VkContext itself")
    for entry in found:
        print(f"  {entry}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
