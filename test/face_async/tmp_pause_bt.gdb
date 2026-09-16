set pagination off
set confirm off
break gst_pad_pause_task
commands
silent
printf "=== PAD_PAUSE_HIT t=%ld ===\n", (long)time(0)
set $p = (GstPad*)$x0
printf "pad=%p\n", $p
p (char*)gst_pad_get_name($p)
set $par = (void*)gst_pad_get_parent($p)
if $par != 0
  p (char*)gst_object_get_name((GstObject*)$par)
  call (void)gst_object_unref((GstObject*)$par)
end
bt 14
continue
end
continue
