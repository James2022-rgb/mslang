#pragma once

#if !defined(MSLANG_ENABLE_MPROF)
# error "Sanity check: MSLANG_ENABLE_MPROF must be defined"
#endif

#if MSLANG_ENABLE_MPROF
# include "mprof/public/tracy.h"
#endif

#if defined(TRACY_ENABLE)
# define MSLANG_INTERNAL_TRACY_ENABLE TRACY_ENABLE
#else
# define MSLANG_INTERNAL_TRACY_ENABLE 0
#endif

#if MSLANG_ENABLE_MPROF && MSLANG_INTERNAL_TRACY_ENABLE
# define MSLANG_TracyZoneNamed MPROF_TracyZoneNamed
# define MSLANG_TracyZoneNamedN MPROF_TracyZoneNamedN
# define MSLANG_TracyZoneNamedC MPROF_TracyZoneNamedC
# define MSLANG_TracyZoneNamedNC MPROF_TracyZoneNamedNC

# define MSLANG_TracyZoneTransient MPROF_TracyZoneTransient
# define MSLANG_TracyZoneTransientN MPROF_TracyZoneTransientN
# define MSLANG_TracyZoneTransientNC MPROF_TracyZoneTransientNC

# define MSLANG_TracyZoneScoped MPROF_TracyZoneScoped
# define MSLANG_TracyZoneScopedN MPROF_TracyZoneScopedN
# define MSLANG_TracyZoneScopedC MPROF_TracyZoneScopedC
# define MSLANG_TracyZoneScopedNC MPROF_TracyZoneScopedNC

# define MSLANG_TracyZoneValue MPROF_TracyZoneValue
# define MSLANG_TracyZoneValueV MPROF_TracyZoneValueV
#else
# define MSLANG_TracyZoneNamed
# define MSLANG_TracyZoneNamedN
# define MSLANG_TracyZoneNamedC
# define MSLANG_TracyZoneNamedNC

# define MSLANG_TracyZoneTransient
# define MSLANG_TracyZoneTransientN
# define MSLANG_TracyZoneTransientNC

# define MSLANG_TracyZoneScoped
# define MSLANG_TracyZoneScopedN
# define MSLANG_TracyZoneScopedC
# define MSLANG_TracyZoneScopedNC

# define MSLANG_TracyZoneValue
# define MSLANG_TracyZoneValueV
#endif
