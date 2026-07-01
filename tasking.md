# Tasking

The firmware keeps display work on the main task. The main task owns page
selection, frame rendering, and all calls into `display_port`.

Button GPIO interrupts do not draw or call network code. They only record pending
navigation state under `button_lock`. The main task consumes that state between
display operations.

Server polling runs in the `server_poll` task. It calls `clock_client_poll()`,
updates `image_store`, and publishes a pending redraw flag for the main task.
It never touches the display.

`image_store` protects appointment images with a recursive mutex. Rendering holds
that mutex while it reads image pointers into the frame buffer. The poll task
waits for that short render window before replacing image memory.

E-paper refreshes still block while the panel busy pin is asserted. Button clicks
during a refresh are queued by the ISR and applied after the refresh completes.
