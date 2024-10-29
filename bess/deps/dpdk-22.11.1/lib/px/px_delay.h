#ifndef PX_DELAY_H_
#define PX_DELAY_H_
#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*---------------------------------------------------------------------------*/
/* ensure that end and start are not NULL before call this function */
static inline unsigned int
get_nsec_diff(struct timespec *end,
              struct timespec *start)
{
    return (end->tv_sec - start->tv_sec) * E9 +
           (end->tv_nsec - start->tv_nsec);
}
/*---------------------------------------------------------------------------*/
/* ensure that item and timestamp are not NULL before call this function */
static inline void
update_timestamp(struct timespec *orig_timestamp,
                 struct timespec *new_timestamp)
{
    /* original item is older */
    if (orig_timestamp->tv_sec < new_timestamp->tv_sec)
    {
        return;
    }

    /* new item is older */
    if (orig_timestamp->tv_sec > new_timestamp->tv_sec)
    {
        *orig_timestamp = *new_timestamp;
        return;
    }

    /* new item is older */
    if (orig_timestamp->tv_nsec < new_timestamp->tv_nsec)
    {
        orig_timestamp->tv_nsec = new_timestamp->tv_nsec;
        return;
    }

    return;
}
/*---------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_DELAY_H_