/* Leak detection is out of scope for this fleet (SPEC 6.2 item 15); ASan + UBSan stay on. */
int __lsan_is_turned_off(void) { return 1; }
