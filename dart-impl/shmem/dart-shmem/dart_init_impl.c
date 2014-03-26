
#include "dart.h"

#include "dart_initialization.h"
#include "dart_init_shmem.h"

dart_ret_t dart_init(int *argc, char ***argv)
{
  if( !argc || !argv ) {
    return DART_ERR_INVAL;
  }
  dart_init_shmem(argc, argv);
  return DART_OK;
}

dart_ret_t dart_exit()
{
  dart_ret_t ret;
  
  dart_barrier(DART_TEAM_ALL);

  ret = dart_exit_shmem();
  return ret;
}
