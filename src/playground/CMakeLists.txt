# Mikael's playground
fau_example(mlund-cosan "./mlund" cosan.cpp)
fau_example(mlund-test "./mlund" test.cpp)
fau_example(mlund-cluster "./mlund" cluster.cpp)
fau_example(mlund-manybody "./mlund" manybody.cpp)
set_target_properties( mlund-test PROPERTIES COMPILE_DEFINITIONS "SOMETHING")

if(ENABLE_MPI AND MPI_CXX_FOUND)
  fau_example(mpitest "./mlund" mpitest.cpp)
endif()

# Anil's playground
fau_example(akurut-manybody "./akurut" manybody.cpp)
fau_example(akurut-gouychapman "./akurut" gouychapman.cpp)
fau_example(akurut-gouychapman-slit "./akurut" gouychapman.cpp)
set_target_properties( akurut-gouychapman-slit PROPERTIES COMPILE_DEFINITIONS "SLIT")

# Robert's playground
fau_cigarexample(cigar "./vacha" cigar.cpp)
set_target_properties( cigar PROPERTIES COMPILE_DEFINITIONS "CIGARPARTICLE" EXCLUDE_FROM_ALL TRUE)
