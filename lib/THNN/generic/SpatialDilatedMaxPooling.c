#ifndef TH_GENERIC_FILE
#define TH_GENERIC_FILE "generic/SpatialDilatedMaxPooling.c"
#else

static void THNN_(SpatialDilatedMaxPooling_updateOutput_frame)(
          real *input_p,
          real *output_p,
          THIndex_t *ind_p,
          long nslices,
          long iwidth,
          long iheight,
          long owidth,
          long oheight,
          int kW,
          int kH,
          int dW,
          int dH,
          int padW,
          int padH,
          int dilationW,
          int dilationH
          )
{
  long k;
#pragma omp parallel for private(k)
  for (k = 0; k < nslices; k++)
  {
    /* loop over output */
    long i, j;
    real *ip = input_p   + k*iwidth*iheight;
    for(i = 0; i < oheight; i++)
    {
      for(j = 0; j < owidth; j++)
      {
        long hstart = i * dH - padH;
        long wstart = j * dW - padW;
        long hend = fminf(hstart + (kH - 1) * dilationH + 1, iheight);
        long wend = fminf(wstart + (kW - 1) * dilationW + 1, iwidth);
        while(hstart < 0)
          hstart += dilationH;
        while(wstart < 0)
          wstart += dilationW;

        /* local pointers */
        real *op = output_p  + k*owidth*oheight + i*owidth + j;
        THIndex_t *indp = (ind_p)   + k*owidth*oheight + i*owidth + j;

        /* compute local max: */
        long maxindex = -1;
        real maxval = -THInf;
        long tcntr = 0;
        long x,y;
        for(y = hstart; y < hend; y += dilationH)
        {
          for(x = wstart; x < wend; x += dilationW)
          {
            tcntr = y*iwidth + x;
            real val = *(ip + tcntr);
            if (val > maxval)
            {
              maxval = val;
              maxindex = tcntr;
            }
          }
        }

        /* set output to local max */
        *op = maxval;

        /* store location of max */
        *indp = maxindex + TH_INDEX_BASE;
      }
    }
  }
}

void THNN_(SpatialDilatedMaxPooling_updateOutput)(
          THNNState *state,
          THTensor *input,
          THTensor *output,
          THIndexTensor *indices,
          int kW,
          int kH,
          int dW,
          int dH,
          int padW,
          int padH,
          int dilationW,
          int dilationH,
          bool ceil_mode)
{
  int dimw = 2;
  int dimh = 1;
  long nbatch = 1;
  long nslices;
  long iheight;
  long iwidth;
  long oheight;
  long owidth;
  real *input_data;
  real *output_data;
  THIndex_t *indices_data;


  THNN_ARGCHECK(input->nDimension == 3 || input->nDimension == 4, 2, input,
		"3D or 4D (batch mode) tensor expected for input, but got: %s");

  if (input->nDimension == 4)
  {
    nbatch = input->size[0];
    dimw++;
    dimh++;
  }
  THArgCheck(input->size[dimw] >= kW - padW && input->size[dimh] >= kH - padH, 2,
	     "input image (H: %d, W: %d) smaller than kernel "
	     "size - padding( kH: %d padH: %d kW: %d padW: %d",
	     input->size[dimh], input->size[dimw], kH, padH, kW, padW);
  THArgCheck(kW/2 >= padW && kH/2 >= padH, 2,
	     "pad should be smaller than half of kernel size, but got "
	     "padW = %d, padH = %d, kW = %d, kH = %d",
	     padW, padH, kW, kH);

  /* sizes */
  nslices = input->size[dimh-1];
  iheight = input->size[dimh];
  iwidth = input->size[dimw];
  if (ceil_mode)
  {
    oheight = (long)(ceil((float)(iheight - (dilationH * (kH - 1) + 1) + 2*padH) / dH)) + 1;
    owidth  = (long)(ceil((float)(iwidth  - (dilationW * (kW - 1) + 1) + 2*padW) / dW)) + 1;
  }
  else
  {
    oheight = (long)(floor((float)(iheight - (dilationH * (kH - 1) + 1) + 2*padH) / dH)) + 1;
    owidth  = (long)(floor((float)(iwidth  - (dilationW * (kW - 1) + 1) + 2*padW) / dW)) + 1;
  }

  if (owidth < 1 || oheight < 1)
    THError("Given input size: (%dx%dx%d). "
	    "Calculated output size: (%dx%dx%d). Output size is too small",
            nslices,iheight,iwidth,nslices,oheight,owidth);

  if (padW || padH)
  {
    // ensure that the last pooling starts inside the image
    if ((oheight - 1)*dH >= iheight + padH)
      --oheight;
    if ((owidth  - 1)*dW >= iwidth  + padW)
      --owidth;
  }

  /* get contiguous input */
  input = THTensor_(newContiguous)(input);

  /* resize output */
  if (input->nDimension == 3)
  {
    THTensor_(resize3d)(output, nslices, oheight, owidth);
    /* indices will contain the locations for each output point */
    THIndexTensor_(resize3d)(indices,  nslices, oheight, owidth);

    input_data = THTensor_(data)(input);
    output_data = THTensor_(data)(output);
    indices_data = THIndexTensor_(data)(indices);

    THNN_(SpatialDilatedMaxPooling_updateOutput_frame)
      (input_data, output_data,
       indices_data,
       nslices,
       iwidth, iheight,
       owidth, oheight,
       kW, kH, dW, dH,
       padW, padH,
       dilationW, dilationH
       );
  }
  else
  {
    long p;

    THTensor_(resize4d)(output, nbatch, nslices, oheight, owidth);
    /* indices will contain the locations for each output point */
    THIndexTensor_(resize4d)(indices, nbatch, nslices, oheight, owidth);

    input_data = THTensor_(data)(input);
    output_data = THTensor_(data)(output);
    indices_data = THIndexTensor_(data)(indices);

#pragma omp parallel for private(p)
    for (p = 0; p < nbatch; p++)
    {
      THNN_(SpatialDilatedMaxPooling_updateOutput_frame)
	(input_data+p*nslices*iwidth*iheight,
	 output_data+p*nslices*owidth*oheight,
	 indices_data+p*nslices*owidth*oheight,
	 nslices,
	 iwidth, iheight,
	 owidth, oheight,
	 kW, kH, dW, dH,
	 padW, padH,
	 dilationW, dilationH
	 );
    }
  }

  /* cleanup */
  THTensor_(free)(input);
}

static void THNN_(SpatialDilatedMaxPooling_updateGradInput_frame)(
          real *gradInput_p,
          real *gradOutput_p,
          THIndex_t *ind_p,
          long nslices,
          long iwidth,
          long iheight,
          long owidth,
          long oheight,
          int dW,
          int dH)
{
  long k;
#pragma omp parallel for private(k)
  for (k = 0; k < nslices; k++)
  {
    real *gradInput_p_k = gradInput_p + k*iwidth*iheight;
    real *gradOutput_p_k = gradOutput_p + k*owidth*oheight;
    THIndex_t *ind_p_k = (ind_p) + k*owidth*oheight;

    /* calculate max points */
    long i, j;
    for(i = 0; i < oheight; i++)
    {
      for(j = 0; j < owidth; j++)
      {
        /* retrieve position of max */
        long maxp = ind_p_k[i*owidth + j] - TH_INDEX_BASE;
        /* update gradient */
        //printf("about to gradInput %i %i %i %i\n", maxp, TH_INDEX_BASE, i*owidth +j, ind_p_k[i*owidth+j]);
        gradInput_p_k[maxp] += gradOutput_p_k[i*owidth + j];
        printf("CPU gradInput: %f\n", gradInput_p_k[maxp]);
        //printf("done gradInput\n");
      }
    }
  }
}

void THNN_(SpatialDilatedMaxPooling_updateGradInput)(
          THNNState *state,
          THTensor *input,
          THTensor *gradOutput,
          THTensor *gradInput,
          THIndexTensor *indices,
          int kW,
          int kH,
          int dW,
          int dH,
          int padW,
          int padH,
          int dilationW,
          int dilationH,
          bool ceil_mode)
{
  int dimw = 2;
  int dimh = 1;
  long nbatch = 1;
  int nslices;
  int iheight;
  int iwidth;
  int oheight;
  int owidth;
  real *gradInput_data;
  real *gradOutput_data;
  THIndex_t *indices_data;

  // TODO: shape check gradOutput

  /* get contiguous gradOutput */
  gradOutput = THTensor_(newContiguous)(gradOutput);

  /* resize */
  THTensor_(resizeAs)(gradInput, input);
  THTensor_(zero)(gradInput);

  if (input->nDimension == 4) {
    nbatch = input->size[0];
    dimw++;
    dimh++;
  }

  /* sizes */
  nslices = input->size[dimh-1];
  iheight = input->size[dimh];
  iwidth = input->size[dimw];
  oheight = gradOutput->size[dimh];
  owidth = gradOutput->size[dimw];

  /* get raw pointers */
  gradInput_data = THTensor_(data)(gradInput);
  gradOutput_data = THTensor_(data)(gradOutput);
  indices_data = THIndexTensor_(data)(indices);

  /* backprop */
  if (input->nDimension == 3)
  {
    printf("!!CPU nDimension3\n");
    THNN_(SpatialDilatedMaxPooling_updateGradInput_frame)
      (gradInput_data, gradOutput_data,
       indices_data,
       nslices,
       iwidth, iheight,
       owidth, oheight,
       dW, dH);
  }
  else
  {
    long p;
#pragma omp parallel for private(p)
    for (p = 0; p < nbatch; p++)
    {
      THNN_(SpatialDilatedMaxPooling_updateGradInput_frame)
	(gradInput_data+p*nslices*iwidth*iheight,
	 gradOutput_data+p*nslices*owidth*oheight,
	 indices_data+p*nslices*owidth*oheight,
	 nslices,
	 iwidth, iheight,
	 owidth, oheight,
	 dW, dH);
    }
  }

  /* cleanup */
  THTensor_(free)(gradOutput);
}

#endif
