#include "compat.h"
#include "userland.h"
#include "audiovar.h"
#include <errno.h>

extern const struct fileops audio_fileops;

struct proc curproc0;
struct proc *curproc = &curproc0;;
struct lwp curlwp0;
struct lwp *curlwp = &curlwp0;
kmutex_t proc_lock0;
kmutex_t *proc_lock = &proc_lock0;

int	fnullop_fcntl(struct file *file, u_int name, void *val)
{
	return 0;
}
void	fnullop_restart(struct file *file)
{
}
