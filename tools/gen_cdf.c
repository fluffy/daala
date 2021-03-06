/*Daala video codec
Copyright (c) 2012 Daala project contributors.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define MAXN 32768

int main(int argc, char **argv)
{
  int i;
  int N;
  int shift;
  int j;
  float p[16];
  int pi[16];
  int sum;
  int aN[MAXN];

  if (argc!=3) fprintf(stderr, "bad bad bad\n");
  N=atoi(argv[1]);
  shift=atoi(argv[2]);
  printf("/* This file is auto-generated using \"gen_cdf %d %d\" */\n\n",
   N, shift);
  printf("#include \"pvq_code.h\"\n\n");
  printf("const ogg_uint16_t cdf_table[%d][16] = {\n", N+1);
  printf("  {");
  for(j=0;j<16;j++){
    printf("%5d%s",32768U-15+j,j+1<16?",":"");
  }
  printf("},\n");
  aN[0]=1;
  for (i=1;i<=N;i++){
    float Ex;
    float gamma;
    float a;
    int cdf;
    float maxp;
    int maxj;
    Ex=(float)i/(1<<shift);
    gamma = (sqrt(1+4*Ex*Ex)-1)/(2*Ex);
    a=-.5/log(gamma);
    aN[i] = floor(.5+256*exp(-1./a));
    /*printf("%f %f ", Ex, a);*/
    p[0]=1-exp(-.5/a);
    for(j=1;j<15;j++)
      p[j]=exp(-.5/a)*(exp(-((float)j-1.)/a)-exp(-(float)j/a));
    p[15]=exp(-.5/a)*exp(-14./a);

    sum=0;
    maxp=0;
    maxj=0;
    for(j=0;j<16;j++)
    {
      if (p[j]>maxp)
      {
        maxp=p[j];
        maxj=j;
      }
      pi[j] = floor(.5+32768*p[j]);
      if (pi[j]==0)
        pi[j]=1;
      sum += pi[j];
    }
    pi[maxj] += 32768-sum;


    cdf = 0;
    printf("  {");
    for(j=0;j<16;j++)
    {
      cdf += pi[j];
      printf("%5d%s", cdf, j+1<16?",":"");
    }
    printf("}%s\n",i+1<=N?",":"");
    /*printf("\n");*/
  }
  printf("};\n");
  printf("\n\n");
  printf("const unsigned char decayE[%d] = {\n", N+1);
  for(i=0;i<=N;i++)
    printf("  %d%s\n", aN[i], i+1<=N?",":"");
  printf("};\n");
  return 0;
}
