include(Linking)
include(Prebuilt)
 if (WINDOWS)
   set(LEAPMOTION_LIBRARY
    optimized ${ARCH_PREBUILT_DIRS_RELEASE}/Leap.lib
    )
elseif (DARWIN)
  set(LEAPMOTION_LIBRARY
    optimized ${ARCH_PREBUILT_DIRS_RELEASE}/libLeap.dylib
    )
endif (WINDOWS)
