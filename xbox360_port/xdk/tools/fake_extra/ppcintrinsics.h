/* transpile-only stand-in for the XDK intrinsics header: prototypes only,
 * so the parser sees calls. cl.exe compiles against the real header. */
void __lwsync(void);
void __sync(void);
void __eieio(void);
unsigned long long __mftb(void);
