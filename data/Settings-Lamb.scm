; Settings-Lamb.scm -- auto-written by the VM.  Reboot required.
; boot_pending 1 means a boot was attempted with these values and did not finish (B441).
; B859 cell_block_size override: requested 8192, EFFECTIVE 24576 -- the value below is the REQUEST and is not what the heap was built with.
; The knob that binds the initial block below the pool target is gc_pool_target, not this one.
((cell_block_size . 8192)
 (gc_pool_target . 24576)
 (extension_block_size . 4096)
 (ncg_frame_pool_size . 16)
 (va_list_max . 8192)
 (gc_total_marked . 12288)
 (sint_in_iram . 1)
 (rootstack_in_iram . 1)
 (boot_pending . 0))
