include(Linking)
include(Prebuilt)

if (DARWIN)
  set(LEAPMOTION_LIBRARY
    optimized ${ARCH_PREBUILT_DIRS_RELEASE}/libLeap.dylib
    )
endif (DARWIN)
