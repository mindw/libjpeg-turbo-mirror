/*
 * jdcoefct.c
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1994-1997, Thomas G. Lane.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2010, D. R. Commander.
 * Copyright (C) 2012-2013, MulticoreWare Inc.
 * For conditions of distribution and use, see the accompanying README file.
 *
 * This file contains the coefficient buffer controller for decompression.
 * This controller is the top level of the JPEG decompressor proper.
 * The coefficient buffer lies between entropy decoding and inverse-DCT steps.
 *
 * In buffered-image mode, this controller is the interface between
 * input-oriented processing and output-oriented processing.
 * Also, the input side (only) is used when reading a file for transcoding.
 */

#define JPEG_INTERNALS
#include "jinclude.h"
#include "jpeglib.h"
#include "jpegcomp.h"

#ifdef WITH_OPENCL_DECODING_SUPPORTED
#include "CL/opencl.h"
#include "joclinit.h"
#include "jocldec.h"
#endif

/* Block smoothing is only applicable for progressive JPEG, so: */
#ifndef D_PROGRESSIVE_SUPPORTED
#undef BLOCK_SMOOTHING_SUPPORTED
#endif

/* Private buffer controller object */

typedef struct {
  struct jpeg_d_coef_controller pub; /* public fields */

  /* These variables keep track of the current location of the input side. */
  /* cinfo->input_iMCU_row is also used for this. */
  JDIMENSION MCU_ctr;		/* counts MCUs processed in current row */
  int MCU_vert_offset;		/* counts MCU rows within iMCU row */
  int MCU_rows_per_iMCU_row;	/* number of such rows needed */

  /* The output side's location is represented by cinfo->output_iMCU_row. */

  /* In single-pass modes, it's sufficient to buffer just one MCU.
   * We allocate a workspace of D_MAX_BLOCKS_IN_MCU coefficient blocks,
   * and let the entropy decoder write into that workspace each time.
   * (On 80x86, the workspace is FAR even though it's not really very big;
   * this is to keep the module interfaces unchanged when a large coefficient
   * buffer is necessary.)
   * In multi-pass modes, this array points to the current MCU's blocks
   * within the virtual arrays; it is used only by the input side.
   */
  JBLOCKROW MCU_buffer[D_MAX_BLOCKS_IN_MCU];

  /* Temporary workspace for one MCU */
  JCOEF * workspace;

#ifdef D_MULTISCAN_FILES_SUPPORTED
  /* In multi-pass modes, we need a virtual block array for each component. */
  jvirt_barray_ptr whole_image[MAX_COMPONENTS];
#endif

#ifdef BLOCK_SMOOTHING_SUPPORTED
  /* When doing block smoothing, we latch coefficient Al values here */
  int * coef_bits_latch;
#define SAVED_COEFS  6		/* we save coef_bits[0..5] */
#endif
} my_coef_controller;

typedef my_coef_controller * my_coef_ptr;

/* Forward declarations */
METHODDEF(int) decompress_onepass
	JPP((j_decompress_ptr cinfo, JSAMPIMAGE output_buf));
#ifdef D_MULTISCAN_FILES_SUPPORTED
METHODDEF(int) decompress_data
	JPP((j_decompress_ptr cinfo, JSAMPIMAGE output_buf));
#endif
#ifdef BLOCK_SMOOTHING_SUPPORTED
LOCAL(boolean) smoothing_ok JPP((j_decompress_ptr cinfo));
METHODDEF(int) decompress_smooth_data
	JPP((j_decompress_ptr cinfo, JSAMPIMAGE output_buf));
#endif


LOCAL(void)
start_iMCU_row (j_decompress_ptr cinfo)
/* Reset within-iMCU-row counters for a new row (input side) */
{
  my_coef_ptr coef = (my_coef_ptr) cinfo->coef;

  /* In an interleaved scan, an MCU row is the same as an iMCU row.
   * In a noninterleaved scan, an iMCU row has v_samp_factor MCU rows.
   * But at the bottom of the image, process only what's left.
   */
  if (cinfo->comps_in_scan > 1) {
    coef->MCU_rows_per_iMCU_row = 1;
  } else {
    if (cinfo->input_iMCU_row < (cinfo->total_iMCU_rows-1))
      coef->MCU_rows_per_iMCU_row = cinfo->cur_comp_info[0]->v_samp_factor;
    else
      coef->MCU_rows_per_iMCU_row = cinfo->cur_comp_info[0]->last_row_height;
  }

  coef->MCU_ctr = 0;
  coef->MCU_vert_offset = 0;
}


/*
 * Initialize for an input processing pass.
 */

METHODDEF(void)
start_input_pass (j_decompress_ptr cinfo)
{
  cinfo->input_iMCU_row = 0;
  start_iMCU_row(cinfo);
}


/*
 * Initialize for an output processing pass.
 */

METHODDEF(void)
start_output_pass (j_decompress_ptr cinfo)
{
#ifdef BLOCK_SMOOTHING_SUPPORTED
  my_coef_ptr coef = (my_coef_ptr) cinfo->coef;

  /* If multipass, check to see whether to use block smoothing on this pass */
  if (coef->pub.coef_arrays != NULL) {
    if (cinfo->do_block_smoothing && smoothing_ok(cinfo))
      coef->pub.decompress_data = decompress_smooth_data;
    else
      coef->pub.decompress_data = decompress_data;
  }
#endif
  cinfo->output_iMCU_row = 0;
}


/*
 * Decompress and return some data in the single-pass case.
 * Always attempts to emit one fully interleaved MCU row ("iMCU" row).
 * Input and output must run in lockstep since we have only a one-MCU buffer.
 * Return value is JPEG_ROW_COMPLETED, JPEG_SCAN_COMPLETED, or JPEG_SUSPENDED.
 *
 * NB: output_buf contains a plane for each component in image,
 * which we index according to the component's SOF position.
 */

METHODDEF(int)
decompress_onepass (j_decompress_ptr cinfo, JSAMPIMAGE output_buf)
{
  my_coef_ptr coef = (my_coef_ptr) cinfo->coef;
  JDIMENSION MCU_col_num;	/* index of current MCU within row */
  JDIMENSION last_MCU_col = cinfo->MCUs_per_row - 1;
  JDIMENSION last_iMCU_row = cinfo->total_iMCU_rows - 1;
  int blkn, ci, xindex, yindex, yoffset, useful_width;
  JSAMPARRAY output_ptr;
  JDIMENSION start_col, output_col;
  jpeg_component_info *compptr;
  inverse_DCT_method_ptr inverse_DCT;

#ifdef WITH_OPENCL_DECODING_SUPPORTED
  int mcuy  = 0, index = 0;
  int block = cinfo->MCUs_per_row;
  int i, j, upfactor,buffer_index, buffer_index_next;
  int qutable[128];
  OCL_STATUS * ocl_status =  (OCL_STATUS* )cinfo->jocl_openClinfo;
  float  qutable_float[128];
  short* qutable_short_inter = (short *)ocl_status->jocl_global_data_ptr_qutable;
  int*   qutable_int_inter = (int *)ocl_status->jocl_global_data_ptr_qutable;
  cl_event buffer_event[BUFFERNUMS];
  int mcunum_buffer = MCUNUMS / cinfo->MCUs_per_row *  cinfo->MCUs_per_row;
  unsigned long buffer_output_size = jocl_cl_get_buffer_unit_size(ocl_status,cinfo->image_width,cinfo->image_height);

  /*offset_input is used to compute the offset of input and output for Kernel*/
  unsigned int offset_input = 0;
  unsigned int mcudecoder   = 0;
  int rows_per_iMCU_row     = coef->MCU_rows_per_iMCU_row;
  int decodeMCU = 0;
  int total_mcu_num = cinfo->total_iMCU_rows * rows_per_iMCU_row * cinfo->MCUs_per_row;
  int buffer_flag = 0;
  cl_int err_code;
  int num_buffer_inter = 10;
  int num_buffer = BUFFERNUMS / num_buffer_inter;
  cl_bool version_ocl = jocl_cl_get_ocl_version(ocl_status);
  /*IDCT FAST SHORT*/
  static const int aanscales[DCTSIZE2] = {
	  /* precomputed values scaled up by 14 bits */
	  16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
	  22725, 31521, 29692, 26722, 22725, 17855, 12299,  6270,
	  21407, 29692, 27969, 25172, 21407, 16819, 11585,  5906,
	  19266, 26722, 25172, 22654, 19266, 15137, 10426,  5315,
	  16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
	  12873, 17855, 16819, 15137, 12873, 10114,  6967,  3552,
	   8867, 12299, 11585, 10426,  8867,  6967,  4799,  2446,
	   4520,  6270,  5906,  5315,  4520,  3552,  2446,  1247
  };
  /*IDCT FAST FLOAT */
  static const double aanscalefactor[8] = {1.0, 1.387039845, 1.306562965, 1.175875602,
                                           1.0, 0.785694958, 0.541196100, 0.275899379};
  /* Determine whether the OpenCL decoding will be used.*/
  if (jocl_cl_is_support_opencl(ocl_status) && jocl_cl_get_decode_support(ocl_status)&& jocl_cl_is_opencl_decompress(cinfo))
    jocl_cl_set_opencl_success(ocl_status);
  else {
    jocl_cl_set_opencl_failure(ocl_status);
    cinfo->opencl_rgb_flag = FALSE;
  }

  if (CL_TRUE == jocl_cl_is_available(ocl_status)) {
    if (cinfo->input_iMCU_row == 0) {
      switch (cinfo->dct_method){
	    case JDCT_IFAST:{
	      /*IDCT FAST INT*/
          for (i = 0; i < DCTSIZE2; ++i) {
            qutable[i] = cinfo->quant_tbl_ptrs[0]->quantval[i];
          }
          if (cinfo->quant_tbl_ptrs[1]->quantval != NULL) {
            for (i = 0; i < DCTSIZE2; ++i) {
              qutable[i + DCTSIZE2] = cinfo->quant_tbl_ptrs[1]->quantval[i];
            }
          }
          else {
            for (i = 0; i < DCTSIZE2; ++i) {
              qutable[i + DCTSIZE2] = cinfo->quant_tbl_ptrs[0]->quantval[i];
            }
          }
          for(i = 0; i < DCTSIZE ; ++i)
            for(j = 0 ; j < DCTSIZE ; ++j) {
              qutable_short_inter[j*8+i] = ((qutable[i*8+j] * aanscales[i*8+j]) +
                (1 << ((12)-1))) >> 12;
              qutable_short_inter[DCTSIZE2+j*8+i] = ((qutable[DCTSIZE2 + i*8+j] *
                aanscales[i*8+j]) + (1 << ((12)-1))) >> 12;
	      }
		  break;
	    }
	    case JDCT_FLOAT:{
	      /*IDCT FAST FLOAT*/ 
          for (i = 0; i < DCTSIZE2; ++i) {
            qutable_float[i] = cinfo->quant_tbl_ptrs[0]->quantval[i];
          }
          if (cinfo->quant_tbl_ptrs[1]->quantval != NULL) {
            for (i = 0; i < DCTSIZE2; ++i) {
              qutable_float[i + DCTSIZE2] = cinfo->quant_tbl_ptrs[1]->quantval[i];
            }
          }
          else {
            for (i = 0; i < DCTSIZE2; ++i) {
              qutable_float[i + DCTSIZE2] = cinfo->quant_tbl_ptrs[0]->quantval[i];
            }
          }
	      for (i = 0; i < 8; i++) 
            for (j = 0; j < 8; j++) {
              ocl_status->jocl_global_data_ptr_qutable[j * 8 + i] = (float)(qutable_float[i * 8 + j] *
                aanscalefactor[i] * aanscalefactor[j]);
              ocl_status->jocl_global_data_ptr_qutable[DCTSIZE2 + j * 8 + i] = (float)(qutable_float[DCTSIZE2 + i * 8 + j] *
                aanscalefactor[i] * aanscalefactor[j]);
            }
		  break;
	    }
	    case JDCT_ISLOW: {
	      /*IDCT SLOW INT*/
          for (i = 0; i < DCTSIZE2; ++i) {
            qutable[i] = cinfo->quant_tbl_ptrs[0]->quantval[i];
          }
          if (cinfo->quant_tbl_ptrs[1]->quantval != NULL) {
            for (i = 0; i < DCTSIZE2; ++i) {
              qutable[i + DCTSIZE2] = cinfo->quant_tbl_ptrs[1]->quantval[i];
            }
          }
          else {
            for (i = 0; i < DCTSIZE2; ++i) {
              qutable[i + DCTSIZE2] = cinfo->quant_tbl_ptrs[0]->quantval[i];
            }
          }
          for (i = 0; i < 8; i++)
            for (j = 0; j < 8; j++) {
              qutable_int_inter[j * 8 + i] = qutable[i * 8 + j];
              qutable_int_inter[DCTSIZE2 + j * 8 + i] = qutable[DCTSIZE2 + i * 8 + j];
            }
	      break;
        }
	  }
      if (cinfo->max_v_samp_factor == 2) {
        if (cinfo->max_h_samp_factor == 1)
          upfactor = 5;
        else
          upfactor = 6;
      } 
      else if (cinfo->max_h_samp_factor == 2) {
        upfactor = 4;
      }
      else {
        upfactor = 3;
      }
    }
    for (; cinfo->input_iMCU_row < cinfo->total_iMCU_rows; 
      (cinfo->input_iMCU_row)++) {
      /* Loop to process as much as one whole iMCU row */
      for (yoffset = coef->MCU_vert_offset; yoffset < coef->MCU_rows_per_iMCU_row;
        yoffset++) {
        for (MCU_col_num = coef->MCU_ctr; MCU_col_num <= last_MCU_col;
          MCU_col_num++) {
          if (upfactor == 6 && CL_TRUE == jocl_cl_get_fancy_status(ocl_status)) {
            //printf("H2V2 fancy \n");
            buffer_index = buffer_flag % num_buffer_inter;
            if (buffer_index == (num_buffer_inter - 1)) buffer_index_next = 0;
            else buffer_index_next = buffer_index + 1;
            if (CL_FALSE == jocl_cl_is_nvidia_opencl(ocl_status)&& mcuy == 0) {
		      if (buffer_flag > (num_buffer_inter - 1)) {
               CL_SAFE_CALL0(err_code = jocl_clWaitForEvents(1, &buffer_event[buffer_index]),return CL_FALSE);
              }
            }
#ifndef JOCL_CL_OS_WIN32
            if (CL_FALSE == jocl_cl_is_nvidia_opencl(ocl_status)) {
              if (version_ocl) {
                CL_SAFE_CALL0(ocl_status->jocl_global_data_ptr_input[buffer_index] = (JCOEFPTR)jocl_clEnqueueMapBuffer(
                  jocl_cl_get_command_queue(ocl_status), ocl_status->jocl_global_data_mem_input[buffer_index], CL_TRUE,
                  CL_MAP_WRITE_INVALIDATE_REGION, 0, MCUNUMS * DCTSIZE2 * 6 * sizeof(JCOEF),
                  0, NULL, NULL, &err_code),return CL_FALSE);
              }
              else {
                CL_SAFE_CALL0(ocl_status->jocl_global_data_ptr_input[buffer_index] = (JCOEFPTR)jocl_clEnqueueMapBuffer(
                  jocl_cl_get_command_queue(ocl_status), ocl_status->jocl_global_data_mem_input[buffer_index], CL_TRUE,
                  CL_MAP_WRITE, 0, MCUNUMS * DCTSIZE2 * 6 * sizeof(JCOEF),
                  0, NULL, NULL, &err_code),return CL_FALSE);
              }
            }
#endif
          for (index = 0; index < cinfo->blocks_in_MCU; ++index)
            coef->MCU_buffer[index] = (JBLOCKROW)(ocl_status->jocl_global_data_ptr_input[buffer_index] +
              mcuy++ * DCTSIZE2);

            /* Try to fetch an MCU.  Entropy decoder expects buffer to be zeroed. */
            if (CL_TRUE == jocl_cl_is_nvidia_opencl(ocl_status)) {
              jzero_far((void FAR *) coef->MCU_buffer[0],
	  	        (size_t) (cinfo->blocks_in_MCU * SIZEOF(JBLOCK)));
            }
            if (! (*cinfo->entropy->decode_mcu) (cinfo, coef->MCU_buffer)) {
              /* Suspension forced; update state counters and exit */
              coef->MCU_vert_offset = yoffset;
              coef->MCU_ctr = MCU_col_num;
              return JPEG_SUSPENDED;
            }
 
            decodeMCU++;
            mcudecoder++;
            if (
#ifdef OPENCL_PIPELINE
              0 == decodeMCU % mcunum_buffer ||
#endif
              (decodeMCU == cinfo->total_iMCU_rows *
              coef->MCU_rows_per_iMCU_row * (last_MCU_col+1))) {
              jocldec_run_kernels_h2v2_fancy(cinfo,
                                             upfactor,
                                             mcudecoder,
                                             block,
                                             offset_input,
                                             total_mcu_num,
                                             decodeMCU,
                                             buffer_index,
                                             buffer_event,
                                             mcunum_buffer);
              offset_input += mcudecoder;
              mcudecoder = 0;
              mcuy = 0;
              buffer_flag ++;
            }
          }
          else {
            buffer_index = buffer_flag % BUFFERNUMS;
            if (buffer_index == 9) buffer_index_next = 0;
            else buffer_index_next = buffer_index + 1;
            if (CL_FALSE == jocl_cl_is_nvidia_opencl(ocl_status) && (CL_FALSE == jocl_cl_get_fancy_status(ocl_status) ||
              upfactor != 4)) {
		      if (buffer_flag > (BUFFERNUMS - 1) && mcuy == 0) {
               CL_SAFE_CALL0(err_code = jocl_clWaitForEvents(1, &buffer_event[buffer_index]), return CL_FALSE);
              }
            }
#ifndef JOCL_CL_OS_WIN32
            if (CL_FALSE == jocl_cl_is_nvidia_opencl(ocl_status)) {
              if (version_ocl) {
                CL_SAFE_CALL0(ocl_status->jocl_global_data_ptr_input[buffer_index] = (JCOEFPTR)jocl_clEnqueueMapBuffer(
                  jocl_cl_get_command_queue(ocl_status), ocl_status->jocl_global_data_mem_input[buffer_index], CL_TRUE,
                  CL_MAP_WRITE_INVALIDATE_REGION, 0, MCUNUMS * DCTSIZE2 * 6 * sizeof(JCOEF),
                  0, NULL, NULL, &err_code),return CL_FALSE);
              }
              else {
                CL_SAFE_CALL0(ocl_status->jocl_global_data_ptr_input[buffer_index] = (JCOEFPTR)jocl_clEnqueueMapBuffer(
                  jocl_cl_get_command_queue(ocl_status), ocl_status->jocl_global_data_mem_input[buffer_index], CL_TRUE,
                  CL_MAP_WRITE, 0, MCUNUMS * DCTSIZE2 * 6 * sizeof(JCOEF),
                  0, NULL, NULL, &err_code),return CL_FALSE);
              }
            }
#endif
            for (index = 0; index < cinfo->blocks_in_MCU; ++index)
            coef->MCU_buffer[index] = (JBLOCKROW)(ocl_status->jocl_global_data_ptr_input[buffer_index] +
                mcuy++ * DCTSIZE2);

            /* Try to fetch an MCU.  Entropy decoder expects buffer to be zeroed. */
            if (CL_TRUE == jocl_cl_is_nvidia_opencl(ocl_status)) {
              jzero_far((void FAR *) coef->MCU_buffer[0],
	  	        (size_t) (cinfo->blocks_in_MCU * SIZEOF(JBLOCK)));
            }
            if (! (*cinfo->entropy->decode_mcu) (cinfo, coef->MCU_buffer)) {
              /* Suspension forced; update state counters and exit */
              coef->MCU_vert_offset = yoffset;
              coef->MCU_ctr = MCU_col_num;
              return JPEG_SUSPENDED;
            }
            if (CL_TRUE == jocl_cl_get_fancy_status(ocl_status) && upfactor == 4) {
              if (mcuy == cinfo->blocks_in_MCU * MCUNUMS) {
                if (CL_FALSE == jocl_cl_is_nvidia_opencl(ocl_status) && (buffer_flag > (BUFFERNUMS - 2))) {
                  CL_SAFE_CALL0(err_code = jocl_clWaitForEvents(1, &buffer_event[buffer_index_next]),return CL_FALSE);
                }
                memcpy(ocl_status->jocl_global_data_ptr_input[buffer_index_next],ocl_status->jocl_global_data_ptr_input[buffer_index] +
                 (MCUNUMS - 1)* cinfo->blocks_in_MCU * DCTSIZE2, cinfo->blocks_in_MCU * DCTSIZE2 * sizeof(JCOEF));
              }
            }
            decodeMCU++;
            mcudecoder++;
            if (CL_FALSE == jocl_cl_get_fancy_status(ocl_status) || (upfactor != 4 && upfactor != 6)) {
              if (
#ifdef OPENCL_PIPELINE
                decodeMCU % MCUNUMS==0 ||
#endif
                (decodeMCU == cinfo->total_iMCU_rows *
                coef->MCU_rows_per_iMCU_row * (last_MCU_col+1))) {
                jocldec_run_kernels_full_image(cinfo,
                                               upfactor,
                                               mcudecoder,
                                               block,
                                               offset_input,
                                               total_mcu_num,
                                               decodeMCU,
                                               buffer_index,
                                               buffer_event);
                offset_input += mcudecoder;
                mcudecoder = 0;
                mcuy = 0;
                buffer_flag ++;
              }
            }
		    else {
              if (
#ifdef OPENCL_PIPELINE
                mcuy == cinfo->blocks_in_MCU * MCUNUMS ||
#endif
                (decodeMCU == cinfo->total_iMCU_rows *
                coef->MCU_rows_per_iMCU_row * (last_MCU_col+1))) {
                jocldec_run_kernels_full_image(cinfo,
                                               upfactor,
                                               mcudecoder,
                                               block,
                                               offset_input,
                                               total_mcu_num,
                                               decodeMCU,
                                               buffer_index,
                                               buffer_event);
                offset_input ++;
                mcuy = cinfo->blocks_in_MCU;
                mcudecoder = 1;
                buffer_flag ++;
              }
		    }
          }
        }
        /* Completed an MCU row, but perhaps not an iMCU row */
        coef->MCU_ctr = 0;
	  }
      if (cinfo->input_iMCU_row != (cinfo->total_iMCU_rows - 1)) {
	    start_iMCU_row(cinfo);
	  }
    }
	/* Completed the iMCU row, advance counters for next one */
	if (++(cinfo->output_iMCU_row) < cinfo->total_iMCU_rows) {
      (*cinfo->inputctl->finish_input_pass) (cinfo);
      return JPEG_ROW_COMPLETED;
    }
  }
  else {
#endif
  /* Loop to process as much as one whole iMCU row */
  for (yoffset = coef->MCU_vert_offset; yoffset < coef->MCU_rows_per_iMCU_row;
       yoffset++) {
    for (MCU_col_num = coef->MCU_ctr; MCU_col_num <= last_MCU_col;
	 MCU_col_num++) {
      /* Try to fetch an MCU.  Entropy decoder expects buffer to be zeroed. */
      jzero_far((void FAR *) coef->MCU_buffer[0],
		(size_t) (cinfo->blocks_in_MCU * SIZEOF(JBLOCK)));
      if (! (*cinfo->entropy->decode_mcu) (cinfo, coef->MCU_buffer)) {
	/* Suspension forced; update state counters and exit */
	coef->MCU_vert_offset = yoffset;
	coef->MCU_ctr = MCU_col_num;
	return JPEG_SUSPENDED;
      }
      /* Determine where data should go in output_buf and do the IDCT thing.
       * We skip dummy blocks at the right and bottom edges (but blkn gets
       * incremented past them!).  Note the inner loop relies on having
       * allocated the MCU_buffer[] blocks sequentially.
       */
      blkn = 0;			/* index of current DCT block within MCU */
      for (ci = 0; ci < cinfo->comps_in_scan; ci++) {
	compptr = cinfo->cur_comp_info[ci];
	/* Don't bother to IDCT an uninteresting component. */
	if (! compptr->component_needed) {
	  blkn += compptr->MCU_blocks;
	  continue;
	}
	inverse_DCT = cinfo->idct->inverse_DCT[compptr->component_index];
	useful_width = (MCU_col_num < last_MCU_col) ? compptr->MCU_width
						    : compptr->last_col_width;
	output_ptr = output_buf[compptr->component_index] +
	  yoffset * compptr->_DCT_scaled_size;
	start_col = MCU_col_num * compptr->MCU_sample_width;
	for (yindex = 0; yindex < compptr->MCU_height; yindex++) {
	  if (cinfo->input_iMCU_row < last_iMCU_row ||
	      yoffset+yindex < compptr->last_row_height) {
	    output_col = start_col;
	    for (xindex = 0; xindex < useful_width; xindex++) {
	      (*inverse_DCT) (cinfo, compptr,
			      (JCOEFPTR) coef->MCU_buffer[blkn+xindex],
			      output_ptr, output_col);
	      output_col += compptr->_DCT_scaled_size;
	    }
	  }
	  blkn += compptr->MCU_width;
	  output_ptr += compptr->_DCT_scaled_size;
	}
      }
    }
    /* Completed an MCU row, but perhaps not an iMCU row */
    coef->MCU_ctr = 0;
  }
  /* Completed the iMCU row, advance counters for next one */
  cinfo->output_iMCU_row++;
  if (++(cinfo->input_iMCU_row) < cinfo->total_iMCU_rows) {
    start_iMCU_row(cinfo);
    return JPEG_ROW_COMPLETED;
  }
#ifdef WITH_OPENCL_DECODING_SUPPORTED
  }
#endif
  /* Completed the scan */
  (*cinfo->inputctl->finish_input_pass) (cinfo);
  return JPEG_SCAN_COMPLETED;
}

/*
 * Dummy consume-input routine for single-pass operation.
 */

METHODDEF(int)
dummy_consume_data (j_decompress_ptr cinfo)
{
  return JPEG_SUSPENDED;	/* Always indicate nothing was done */
}


#ifdef D_MULTISCAN_FILES_SUPPORTED

/*
 * Consume input data and store it in the full-image coefficient buffer.
 * We read as much as one fully interleaved MCU row ("iMCU" row) per call,
 * ie, v_samp_factor block rows for each component in the scan.
 * Return value is JPEG_ROW_COMPLETED, JPEG_SCAN_COMPLETED, or JPEG_SUSPENDED.
 */

METHODDEF(int)
consume_data (j_decompress_ptr cinfo)
{
  my_coef_ptr coef = (my_coef_ptr) cinfo->coef;
  JDIMENSION MCU_col_num;	/* index of current MCU within row */
  int blkn, ci, xindex, yindex, yoffset;
  JDIMENSION start_col;
  JBLOCKARRAY buffer[MAX_COMPS_IN_SCAN];
  JBLOCKROW buffer_ptr;
  jpeg_component_info *compptr;

#ifdef WITH_OPENCL_DECODING_SUPPORTED
  jocl_cl_set_opencl_failure((OCL_STATUS* )cinfo->jocl_openClinfo);
  cinfo->opencl_rgb_flag = FALSE;
#endif
  /* Align the virtual buffers for the components used in this scan. */
  for (ci = 0; ci < cinfo->comps_in_scan; ci++) {
    compptr = cinfo->cur_comp_info[ci];
    buffer[ci] = (*cinfo->mem->access_virt_barray)
      ((j_common_ptr) cinfo, coef->whole_image[compptr->component_index],
       cinfo->input_iMCU_row * compptr->v_samp_factor,
       (JDIMENSION) compptr->v_samp_factor, TRUE);
    /* Note: entropy decoder expects buffer to be zeroed,
     * but this is handled automatically by the memory manager
     * because we requested a pre-zeroed array.
     */
  }

  /* Loop to process one whole iMCU row */
  for (yoffset = coef->MCU_vert_offset; yoffset < coef->MCU_rows_per_iMCU_row;
       yoffset++) {
    for (MCU_col_num = coef->MCU_ctr; MCU_col_num < cinfo->MCUs_per_row;
	 MCU_col_num++) {
      /* Construct list of pointers to DCT blocks belonging to this MCU */
      blkn = 0;			/* index of current DCT block within MCU */
      for (ci = 0; ci < cinfo->comps_in_scan; ci++) {
	compptr = cinfo->cur_comp_info[ci];
	start_col = MCU_col_num * compptr->MCU_width;
	for (yindex = 0; yindex < compptr->MCU_height; yindex++) {
	  buffer_ptr = buffer[ci][yindex+yoffset] + start_col;
	  for (xindex = 0; xindex < compptr->MCU_width; xindex++) {
	    coef->MCU_buffer[blkn++] = buffer_ptr++;
	  }
	}
      }
      /* Try to fetch the MCU. */
      if (! (*cinfo->entropy->decode_mcu) (cinfo, coef->MCU_buffer)) {
	/* Suspension forced; update state counters and exit */
	coef->MCU_vert_offset = yoffset;
	coef->MCU_ctr = MCU_col_num;
	return JPEG_SUSPENDED;
      }
    }
    /* Completed an MCU row, but perhaps not an iMCU row */
    coef->MCU_ctr = 0;
  }
  /* Completed the iMCU row, advance counters for next one */
  if (++(cinfo->input_iMCU_row) < cinfo->total_iMCU_rows) {
    start_iMCU_row(cinfo);
    return JPEG_ROW_COMPLETED;
  }
  /* Completed the scan */
  (*cinfo->inputctl->finish_input_pass) (cinfo);
  return JPEG_SCAN_COMPLETED;
}


/*
 * Decompress and return some data in the multi-pass case.
 * Always attempts to emit one fully interleaved MCU row ("iMCU" row).
 * Return value is JPEG_ROW_COMPLETED, JPEG_SCAN_COMPLETED, or JPEG_SUSPENDED.
 *
 * NB: output_buf contains a plane for each component in image.
 */

METHODDEF(int)
decompress_data (j_decompress_ptr cinfo, JSAMPIMAGE output_buf)
{
  my_coef_ptr coef = (my_coef_ptr) cinfo->coef;
  JDIMENSION last_iMCU_row = cinfo->total_iMCU_rows - 1;
  JDIMENSION block_num;
  int ci, block_row, block_rows;
  JBLOCKARRAY buffer;
  JBLOCKROW buffer_ptr;
  JSAMPARRAY output_ptr;
  JDIMENSION output_col;
  jpeg_component_info *compptr;
  inverse_DCT_method_ptr inverse_DCT;

  /* Force some input to be done if we are getting ahead of the input. */
  while (cinfo->input_scan_number < cinfo->output_scan_number ||
	 (cinfo->input_scan_number == cinfo->output_scan_number &&
	  cinfo->input_iMCU_row <= cinfo->output_iMCU_row)) {
    if ((*cinfo->inputctl->consume_input)(cinfo) == JPEG_SUSPENDED)
      return JPEG_SUSPENDED;
  }

  /* OK, output from the virtual arrays. */
  for (ci = 0, compptr = cinfo->comp_info; ci < cinfo->num_components;
       ci++, compptr++) {
    /* Don't bother to IDCT an uninteresting component. */
    if (! compptr->component_needed)
      continue;
    /* Align the virtual buffer for this component. */
    buffer = (*cinfo->mem->access_virt_barray)
      ((j_common_ptr) cinfo, coef->whole_image[ci],
       cinfo->output_iMCU_row * compptr->v_samp_factor,
       (JDIMENSION) compptr->v_samp_factor, FALSE);
    /* Count non-dummy DCT block rows in this iMCU row. */
    if (cinfo->output_iMCU_row < last_iMCU_row)
      block_rows = compptr->v_samp_factor;
    else {
      /* NB: can't use last_row_height here; it is input-side-dependent! */
      block_rows = (int) (compptr->height_in_blocks % compptr->v_samp_factor);
      if (block_rows == 0) block_rows = compptr->v_samp_factor;
    }
    inverse_DCT = cinfo->idct->inverse_DCT[ci];
    output_ptr = output_buf[ci];
    /* Loop over all DCT blocks to be processed. */
    for (block_row = 0; block_row < block_rows; block_row++) {
      buffer_ptr = buffer[block_row];
      output_col = 0;
      for (block_num = 0; block_num < compptr->width_in_blocks; block_num++) {
	(*inverse_DCT) (cinfo, compptr, (JCOEFPTR) buffer_ptr,
			output_ptr, output_col);
	buffer_ptr++;
	output_col += compptr->_DCT_scaled_size;
      }
      output_ptr += compptr->_DCT_scaled_size;
    }
  }

  if (++(cinfo->output_iMCU_row) < cinfo->total_iMCU_rows)
    return JPEG_ROW_COMPLETED;
  return JPEG_SCAN_COMPLETED;
}

#endif /* D_MULTISCAN_FILES_SUPPORTED */


#ifdef BLOCK_SMOOTHING_SUPPORTED

/*
 * This code applies interblock smoothing as described by section K.8
 * of the JPEG standard: the first 5 AC coefficients are estimated from
 * the DC values of a DCT block and its 8 neighboring blocks.
 * We apply smoothing only for progressive JPEG decoding, and only if
 * the coefficients it can estimate are not yet known to full precision.
 */

/* Natural-order array positions of the first 5 zigzag-order coefficients */
#define Q01_POS  1
#define Q10_POS  8
#define Q20_POS  16
#define Q11_POS  9
#define Q02_POS  2

/*
 * Determine whether block smoothing is applicable and safe.
 * We also latch the current states of the coef_bits[] entries for the
 * AC coefficients; otherwise, if the input side of the decompressor
 * advances into a new scan, we might think the coefficients are known
 * more accurately than they really are.
 */

LOCAL(boolean)
smoothing_ok (j_decompress_ptr cinfo)
{
  my_coef_ptr coef = (my_coef_ptr) cinfo->coef;
  boolean smoothing_useful = FALSE;
  int ci, coefi;
  jpeg_component_info *compptr;
  JQUANT_TBL * qtable;
  int * coef_bits;
  int * coef_bits_latch;

  if (! cinfo->progressive_mode || cinfo->coef_bits == NULL)
    return FALSE;

  /* Allocate latch area if not already done */
  if (coef->coef_bits_latch == NULL)
    coef->coef_bits_latch = (int *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_IMAGE,
				  cinfo->num_components *
				  (SAVED_COEFS * SIZEOF(int)));
  coef_bits_latch = coef->coef_bits_latch;

  for (ci = 0, compptr = cinfo->comp_info; ci < cinfo->num_components;
       ci++, compptr++) {
    /* All components' quantization values must already be latched. */
    if ((qtable = compptr->quant_table) == NULL)
      return FALSE;
    /* Verify DC & first 5 AC quantizers are nonzero to avoid zero-divide. */
    if (qtable->quantval[0] == 0 ||
	qtable->quantval[Q01_POS] == 0 ||
	qtable->quantval[Q10_POS] == 0 ||
	qtable->quantval[Q20_POS] == 0 ||
	qtable->quantval[Q11_POS] == 0 ||
	qtable->quantval[Q02_POS] == 0)
      return FALSE;
    /* DC values must be at least partly known for all components. */
    coef_bits = cinfo->coef_bits[ci];
    if (coef_bits[0] < 0)
      return FALSE;
    /* Block smoothing is helpful if some AC coefficients remain inaccurate. */
    for (coefi = 1; coefi <= 5; coefi++) {
      coef_bits_latch[coefi] = coef_bits[coefi];
      if (coef_bits[coefi] != 0)
	smoothing_useful = TRUE;
    }
    coef_bits_latch += SAVED_COEFS;
  }

  return smoothing_useful;
}


/*
 * Variant of decompress_data for use when doing block smoothing.
 */

METHODDEF(int)
decompress_smooth_data (j_decompress_ptr cinfo, JSAMPIMAGE output_buf)
{
  my_coef_ptr coef = (my_coef_ptr) cinfo->coef;
  JDIMENSION last_iMCU_row = cinfo->total_iMCU_rows - 1;
  JDIMENSION block_num, last_block_column;
  int ci, block_row, block_rows, access_rows;
  JBLOCKARRAY buffer;
  JBLOCKROW buffer_ptr, prev_block_row, next_block_row;
  JSAMPARRAY output_ptr;
  JDIMENSION output_col;
  jpeg_component_info *compptr;
  inverse_DCT_method_ptr inverse_DCT;
  boolean first_row, last_row;
  JCOEF * workspace;
  int *coef_bits;
  JQUANT_TBL *quanttbl;
  INT32 Q00,Q01,Q02,Q10,Q11,Q20, num;
  int DC1,DC2,DC3,DC4,DC5,DC6,DC7,DC8,DC9;
  int Al, pred;

  /* Keep a local variable to avoid looking it up more than once */
  workspace = coef->workspace;

  /* Force some input to be done if we are getting ahead of the input. */
  while (cinfo->input_scan_number <= cinfo->output_scan_number &&
	 ! cinfo->inputctl->eoi_reached) {
    if (cinfo->input_scan_number == cinfo->output_scan_number) {
      /* If input is working on current scan, we ordinarily want it to
       * have completed the current row.  But if input scan is DC,
       * we want it to keep one row ahead so that next block row's DC
       * values are up to date.
       */
      JDIMENSION delta = (cinfo->Ss == 0) ? 1 : 0;
      if (cinfo->input_iMCU_row > cinfo->output_iMCU_row+delta)
	break;
    }
    if ((*cinfo->inputctl->consume_input)(cinfo) == JPEG_SUSPENDED)
      return JPEG_SUSPENDED;
  }

  /* OK, output from the virtual arrays. */
  for (ci = 0, compptr = cinfo->comp_info; ci < cinfo->num_components;
       ci++, compptr++) {
    /* Don't bother to IDCT an uninteresting component. */
    if (! compptr->component_needed)
      continue;
    /* Count non-dummy DCT block rows in this iMCU row. */
    if (cinfo->output_iMCU_row < last_iMCU_row) {
      block_rows = compptr->v_samp_factor;
      access_rows = block_rows * 2; /* this and next iMCU row */
      last_row = FALSE;
    } else {
      /* NB: can't use last_row_height here; it is input-side-dependent! */
      block_rows = (int) (compptr->height_in_blocks % compptr->v_samp_factor);
      if (block_rows == 0) block_rows = compptr->v_samp_factor;
      access_rows = block_rows; /* this iMCU row only */
      last_row = TRUE;
    }
    /* Align the virtual buffer for this component. */
    if (cinfo->output_iMCU_row > 0) {
      access_rows += compptr->v_samp_factor; /* prior iMCU row too */
      buffer = (*cinfo->mem->access_virt_barray)
	((j_common_ptr) cinfo, coef->whole_image[ci],
	 (cinfo->output_iMCU_row - 1) * compptr->v_samp_factor,
	 (JDIMENSION) access_rows, FALSE);
      buffer += compptr->v_samp_factor;	/* point to current iMCU row */
      first_row = FALSE;
    } else {
      buffer = (*cinfo->mem->access_virt_barray)
	((j_common_ptr) cinfo, coef->whole_image[ci],
	 (JDIMENSION) 0, (JDIMENSION) access_rows, FALSE);
      first_row = TRUE;
    }
    /* Fetch component-dependent info */
    coef_bits = coef->coef_bits_latch + (ci * SAVED_COEFS);
    quanttbl = compptr->quant_table;
    Q00 = quanttbl->quantval[0];
    Q01 = quanttbl->quantval[Q01_POS];
    Q10 = quanttbl->quantval[Q10_POS];
    Q20 = quanttbl->quantval[Q20_POS];
    Q11 = quanttbl->quantval[Q11_POS];
    Q02 = quanttbl->quantval[Q02_POS];
    inverse_DCT = cinfo->idct->inverse_DCT[ci];
    output_ptr = output_buf[ci];
    /* Loop over all DCT blocks to be processed. */
    for (block_row = 0; block_row < block_rows; block_row++) {
      buffer_ptr = buffer[block_row];
      if (first_row && block_row == 0)
	prev_block_row = buffer_ptr;
      else
	prev_block_row = buffer[block_row-1];
      if (last_row && block_row == block_rows-1)
	next_block_row = buffer_ptr;
      else
	next_block_row = buffer[block_row+1];
      /* We fetch the surrounding DC values using a sliding-register approach.
       * Initialize all nine here so as to do the right thing on narrow pics.
       */
      DC1 = DC2 = DC3 = (int) prev_block_row[0][0];
      DC4 = DC5 = DC6 = (int) buffer_ptr[0][0];
      DC7 = DC8 = DC9 = (int) next_block_row[0][0];
      output_col = 0;
      last_block_column = compptr->width_in_blocks - 1;
      for (block_num = 0; block_num <= last_block_column; block_num++) {
	/* Fetch current DCT block into workspace so we can modify it. */
	jcopy_block_row(buffer_ptr, (JBLOCKROW) workspace, (JDIMENSION) 1);
	/* Update DC values */
	if (block_num < last_block_column) {
	  DC3 = (int) prev_block_row[1][0];
	  DC6 = (int) buffer_ptr[1][0];
	  DC9 = (int) next_block_row[1][0];
	}
	/* Compute coefficient estimates per K.8.
	 * An estimate is applied only if coefficient is still zero,
	 * and is not known to be fully accurate.
	 */
	/* AC01 */
	if ((Al=coef_bits[1]) != 0 && workspace[1] == 0) {
	  num = 36 * Q00 * (DC4 - DC6);
	  if (num >= 0) {
	    pred = (int) (((Q01<<7) + num) / (Q01<<8));
	    if (Al > 0 && pred >= (1<<Al))
	      pred = (1<<Al)-1;
	  } else {
	    pred = (int) (((Q01<<7) - num) / (Q01<<8));
	    if (Al > 0 && pred >= (1<<Al))
	      pred = (1<<Al)-1;
	    pred = -pred;
	  }
	  workspace[1] = (JCOEF) pred;
	}
	/* AC10 */
	if ((Al=coef_bits[2]) != 0 && workspace[8] == 0) {
	  num = 36 * Q00 * (DC2 - DC8);
	  if (num >= 0) {
	    pred = (int) (((Q10<<7) + num) / (Q10<<8));
	    if (Al > 0 && pred >= (1<<Al))
	      pred = (1<<Al)-1;
	  } else {
	    pred = (int) (((Q10<<7) - num) / (Q10<<8));
	    if (Al > 0 && pred >= (1<<Al))
	      pred = (1<<Al)-1;
	    pred = -pred;
	  }
	  workspace[8] = (JCOEF) pred;
	}
	/* AC20 */
	if ((Al=coef_bits[3]) != 0 && workspace[16] == 0) {
	  num = 9 * Q00 * (DC2 + DC8 - 2*DC5);
	  if (num >= 0) {
	    pred = (int) (((Q20<<7) + num) / (Q20<<8));
	    if (Al > 0 && pred >= (1<<Al))
	      pred = (1<<Al)-1;
	  } else {
	    pred = (int) (((Q20<<7) - num) / (Q20<<8));
	    if (Al > 0 && pred >= (1<<Al))
	      pred = (1<<Al)-1;
	    pred = -pred;
	  }
	  workspace[16] = (JCOEF) pred;
	}
	/* AC11 */
	if ((Al=coef_bits[4]) != 0 && workspace[9] == 0) {
	  num = 5 * Q00 * (DC1 - DC3 - DC7 + DC9);
	  if (num >= 0) {
	    pred = (int) (((Q11<<7) + num) / (Q11<<8));
	    if (Al > 0 && pred >= (1<<Al))
	      pred = (1<<Al)-1;
	  } else {
	    pred = (int) (((Q11<<7) - num) / (Q11<<8));
	    if (Al > 0 && pred >= (1<<Al))
	      pred = (1<<Al)-1;
	    pred = -pred;
	  }
	  workspace[9] = (JCOEF) pred;
	}
	/* AC02 */
	if ((Al=coef_bits[5]) != 0 && workspace[2] == 0) {
	  num = 9 * Q00 * (DC4 + DC6 - 2*DC5);
	  if (num >= 0) {
	    pred = (int) (((Q02<<7) + num) / (Q02<<8));
	    if (Al > 0 && pred >= (1<<Al))
	      pred = (1<<Al)-1;
	  } else {
	    pred = (int) (((Q02<<7) - num) / (Q02<<8));
	    if (Al > 0 && pred >= (1<<Al))
	      pred = (1<<Al)-1;
	    pred = -pred;
	  }
	  workspace[2] = (JCOEF) pred;
	}
	/* OK, do the IDCT */
	(*inverse_DCT) (cinfo, compptr, (JCOEFPTR) workspace,
			output_ptr, output_col);
	/* Advance for next column */
	DC1 = DC2; DC2 = DC3;
	DC4 = DC5; DC5 = DC6;
	DC7 = DC8; DC8 = DC9;
	buffer_ptr++, prev_block_row++, next_block_row++;
	output_col += compptr->_DCT_scaled_size;
      }
      output_ptr += compptr->_DCT_scaled_size;
    }
  }

  if (++(cinfo->output_iMCU_row) < cinfo->total_iMCU_rows)
    return JPEG_ROW_COMPLETED;
  return JPEG_SCAN_COMPLETED;
}

#endif /* BLOCK_SMOOTHING_SUPPORTED */


/*
 * Initialize coefficient buffer controller.
 */

GLOBAL(void)
jinit_d_coef_controller (j_decompress_ptr cinfo, boolean need_full_buffer)
{
  my_coef_ptr coef;

  coef = (my_coef_ptr)
    (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_IMAGE,
				SIZEOF(my_coef_controller));
  cinfo->coef = (struct jpeg_d_coef_controller *) coef;
  coef->pub.start_input_pass = start_input_pass;
  coef->pub.start_output_pass = start_output_pass;
#ifdef BLOCK_SMOOTHING_SUPPORTED
  coef->coef_bits_latch = NULL;
#endif

  /* Create the coefficient buffer. */
  if (need_full_buffer) {
#ifdef D_MULTISCAN_FILES_SUPPORTED
    /* Allocate a full-image virtual array for each component, */
    /* padded to a multiple of samp_factor DCT blocks in each direction. */
    /* Note we ask for a pre-zeroed array. */
    int ci, access_rows;
    jpeg_component_info *compptr;

    for (ci = 0, compptr = cinfo->comp_info; ci < cinfo->num_components;
	 ci++, compptr++) {
      access_rows = compptr->v_samp_factor;
#ifdef BLOCK_SMOOTHING_SUPPORTED
      /* If block smoothing could be used, need a bigger window */
      if (cinfo->progressive_mode)
	access_rows *= 3;
#endif
      coef->whole_image[ci] = (*cinfo->mem->request_virt_barray)
	((j_common_ptr) cinfo, JPOOL_IMAGE, TRUE,
	 (JDIMENSION) jround_up((long) compptr->width_in_blocks,
				(long) compptr->h_samp_factor),
	 (JDIMENSION) jround_up((long) compptr->height_in_blocks,
				(long) compptr->v_samp_factor),
	 (JDIMENSION) access_rows);
    }
    coef->pub.consume_data = consume_data;
    coef->pub.decompress_data = decompress_data;
    coef->pub.coef_arrays = coef->whole_image; /* link to virtual arrays */
#else
    ERREXIT(cinfo, JERR_NOT_COMPILED);
#endif
  } else {
    /* We only need a single-MCU buffer. */
    JBLOCKROW buffer;
    int i;

    buffer = (JBLOCKROW)
      (*cinfo->mem->alloc_large) ((j_common_ptr) cinfo, JPOOL_IMAGE,
				  D_MAX_BLOCKS_IN_MCU * SIZEOF(JBLOCK));
    for (i = 0; i < D_MAX_BLOCKS_IN_MCU; i++) {
      coef->MCU_buffer[i] = buffer + i;
    }
    coef->pub.consume_data = dummy_consume_data;
    coef->pub.decompress_data = decompress_onepass;
    coef->pub.coef_arrays = NULL; /* flag for no virtual arrays */
  }

  /* Allocate the workspace buffer */
  coef->workspace = (JCOEF *)
    (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_IMAGE,
                                SIZEOF(JCOEF) * DCTSIZE2);
}