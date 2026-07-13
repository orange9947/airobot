"""MicroPython boot-time memory tuning."""

import gc

gc.collect()
if hasattr(gc, "threshold"):
    gc.threshold(gc.mem_free() // 4 + gc.mem_alloc())
