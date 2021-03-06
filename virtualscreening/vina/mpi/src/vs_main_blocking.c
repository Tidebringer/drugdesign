/*
 * Routines to performe virtual screening using mpi
 * These routines were developed by:
 * Rodrigo Antonio Faccioli - rodrigo.faccioli@usp.br / rodrigo.faccioli@gmail.com  
 * Leandro Oliveira Bortot  - leandro.bortot@usp.br / leandro.obt@gmail.com
*/
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "defines.h"
#include "docking.h"
#include "messages.h"
#include "load_parameters.h"
#include "futil.h"
#include "string_owner.h"
#include "mpi_parameters_type.h"
#include "mpi_docking_type.h"
#include "vina.h"
#include "execution_information.h"
#include "docking_log.h"

int main(int argc, char *argv[]) {

 
  MPI_Init(&argc, &argv);	

  //Creating mpi struct types
/*************  mpi_input_parameters_t ***************************/
  const int nitems=8;
  int blocklengths[nitems] = {MAX_PATH, MAX_PATH, MAX_PATH, MAX_PATH, MAX_PATH, MAX_PATH_FILE_NAME, MAX_PATH_FILE_NAME, MAX_PATH_FILE_NAME};
  MPI_Datatype types[nitems] = {MPI_CHAR, MPI_CHAR, MPI_CHAR, MPI_CHAR, MPI_CHAR, MPI_CHAR, MPI_CHAR, MPI_CHAR};  
  MPI_Aint offsets[nitems];

  offsets[0] = offsetof(input_parameters_t, local_execute);
  offsets[1] = offsetof(input_parameters_t, path_receptors);
  offsets[2] = offsetof(input_parameters_t, path_compounds);
  offsets[3] = offsetof(input_parameters_t, path_out);
  offsets[4] = offsetof(input_parameters_t, path_log);
  offsets[5] = offsetof(input_parameters_t, config_vina);
  offsets[6] = offsetof(input_parameters_t, vina_program);
  offsets[7] = offsetof(input_parameters_t, compound_database);

  MPI_Type_create_struct(nitems, blocklengths, offsets, types, &mpi_input_parameters_t);
  MPI_Type_commit(&mpi_input_parameters_t); 
/*************  mpi_input_parameters_t end ***************************/

  int root = 0;
  const int tag_docking = 1;
  int *v_number_dock = NULL;
  int number_dock;
  int world_size;
  int world_rank;
  int nthreads;
  double started_time, finished_time;
  time_t started_date, finished_date;
  MPI_Request request_dock;  

  char *aux_fgets = NULL; //used for getting return of fgets

  MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);
/*
	// It is assuming at least 2 processes for performing the Virtual Screening
  if (world_size < 2) {
    fatal_error("World size must be greater than 1 for performing the Virtual Screening\n");
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
*/
  //Preparing input parameters data structure
  input_parameters_t *param=NULL;
  param = (input_parameters_t*)malloc(sizeof(input_parameters_t));    
  //loading input parameters files
  if (world_rank == root) {    
    load_parameters_from_file(param,argv[1]);        
  }
  //Broadcast input parameters
  MPI_Bcast(param, 1, mpi_input_parameters_t, root, MPI_COMM_WORLD);  

  //Preparing the docking number for each proecess
  if (world_rank == root) {    
    int full_dock;
    full_dock = get_number_docking_from_file(argv[2]);
    v_number_dock = (int*)malloc(sizeof(int)*world_size);
    set_number_docking(v_number_dock, &world_size, &full_dock);
  } 

  //Guarantee that all process received all data
  MPI_Barrier(MPI_COMM_WORLD);

  //Decompose all docking against all process  
  if (world_rank == root){
    FILE *f_dock=NULL;    
    char *line=NULL;
    int num_line_ref;    
    int dock_dist; //represents all docking number
    int r; 
    int i; //represents index for array in docking_by_rank
    docking_t **docking_by_rank = NULL;
    
    //saving information of virtual screening execution
    nthreads = 1;//omp_get_num_threads();
    save_information(param->local_execute, &world_size, v_number_dock, &nthreads);  

    
    //Array of array to store docking per processs
    docking_by_rank = (docking_t**)malloc(sizeof(docking_t)*world_size);
    for (r = 0; r <world_size;r++){
      docking_by_rank[r] = (docking_t*)malloc(sizeof(docking_t)*v_number_dock[r]);
    }

    dock_dist = get_number_docking_from_file(argv[2]);
    line = (char*)malloc(MAX_LINE_FILE);
    f_dock = open_file(argv[2], fREAD);
    //Ignoring first  line of file
    aux_fgets = fgets(line, MAX_LINE_FILE, f_dock);
    num_line_ref=0;
    i = -1;
    while (num_line_ref < dock_dist){
      r = 0;
      i = i + 1; 
      while ( (num_line_ref < dock_dist) && (r < world_size) ){
        aux_fgets = fgets(line, MAX_LINE_FILE, f_dock);
        set_receptor_compound(docking_by_rank[r][i].receptor, 
          docking_by_rank[r][i].compound,
          &docking_by_rank[r][i].num_torsion_angle,
          &docking_by_rank[r][i].num_atom,          
          line);
        num_line_ref = num_line_ref + 1;
        r = r + 1;        
      }
    }  
    fclose(f_dock);
    free(line);

    for (r = 0; r <world_size;r++){
      save_file_docking_from_array(docking_by_rank[r], &v_number_dock[r], param->local_execute, &r); 
    }
    //free docking_by_rank    
    for (r = 0; r <world_size;r++){
      deAllocate_docking(docking_by_rank[r]);  
      docking_by_rank[r] = NULL;
    }
    docking_by_rank = NULL;

  }
  //Guarantee that all process received all data
  MPI_Barrier(MPI_COMM_WORLD);

  //Call Docking from all process
  if (world_rank == root){
    started_time = MPI_Wtime();
    started_date = time(NULL);

    //Obtaining the number of docking 
    docking_t *v_docking = NULL;
    char *f_file_rank = NULL;
    char *path_file_rank = NULL;    
    char *log_file_name = NULL;
    char *path_file_log = NULL;
    time_t f_time_docking, s_time_docking;

    log_file_name = (char*)malloc(sizeof(char)*MAX_FILE_NAME);
    f_file_rank = (char*)malloc(sizeof(char)*MAX_FILE_NAME);
    get_docking_file_name(f_file_rank, &world_rank);
    path_file_rank = path_join_file(param->local_execute, f_file_rank);
    number_dock = get_number_docking_from_file(path_file_rank);
    free(path_file_rank);
    free(f_file_rank);    
    //Preparing log
    build_log_file_name_from_rank(log_file_name, &world_rank);
    path_file_log = path_join_file(param->local_execute, log_file_name);
    free(log_file_name);


    v_docking = (docking_t*)malloc(number_dock*sizeof(docking_t));    
    load_docking_from_file(v_docking, &number_dock, param->local_execute, &world_rank);
    int i;
    initialize_vina_execution();
    initialize_log(path_file_log);
    for (i = 0; i < number_dock; i++){      
      s_time_docking = time(NULL);
      call_vina(param, &v_docking[i]);
      f_time_docking = time(NULL);
      save_log(path_file_log, &v_docking[i], &f_time_docking, &s_time_docking);
    }
    free(path_file_log);    
    finish_vina_execution();    
    deAllocate_docking(v_docking);
  }else{
    docking_t *v_docking = NULL;
    char *f_file_rank = NULL;
    char *path_file_rank = NULL;
    char *log_file_name = NULL;
    char *path_file_log = NULL;
    time_t f_time_docking, s_time_docking;

    //Obtaining the number of docking 
    f_file_rank = (char*)malloc(sizeof(char)*MAX_FILE_NAME);
    get_docking_file_name(f_file_rank, &world_rank);
    path_file_rank = path_join_file(param->local_execute, f_file_rank);
    number_dock = get_number_docking_from_file(path_file_rank);
    free(path_file_rank);
    free(f_file_rank);
    //Preparing log
    log_file_name = (char*)malloc(sizeof(char)*MAX_FILE_NAME);
    build_log_file_name_from_rank(log_file_name, &world_rank);
    path_file_log = path_join_file(param->local_execute, log_file_name);
    free(log_file_name);

    //Allocating array of docking_t
    v_docking = (docking_t*)malloc(number_dock*sizeof(docking_t));    
    load_docking_from_file(v_docking, &number_dock, param->local_execute, &world_rank);
    int i;
    initialize_vina_execution();
    initialize_log(path_file_log);        
    for (i = 0; i < number_dock; i++){
      s_time_docking = time(NULL);
      call_vina(param, &v_docking[i]);
      f_time_docking = time(NULL);
      save_log(path_file_log, &v_docking[i], &f_time_docking, &s_time_docking);
    }
    free(path_file_log);
    finish_vina_execution(); 
    deAllocate_docking(v_docking);
  }

  MPI_Barrier(MPI_COMM_WORLD);
  finished_time = MPI_Wtime();
  finished_date = time(NULL);

  if (world_rank == root){
    saving_time_execution(param->local_execute, &finished_time, &started_time, &finished_date, &started_date);    
  }

  MPI_Type_free(&mpi_input_parameters_t);  
  
  deAllocateload_parameters(param);

  MPI_Finalize();
	return 0;
}