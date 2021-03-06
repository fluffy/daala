#include <float.h>
#include <omp.h>
#include <stdlib.h>
#include <sys/timeb.h>
#include "cholesky.h"
#include "od_defs.h"
#include "od_covmat.h"
#include "od_filter.h"
#include "od_intra.h"
#include "image_tools.h"
#include "stats_tools.h"
#include "svd.h"

#define USE_SVD     (0)
#define BITS_SELECT (0)
#define USE_WEIGHTS (0)
#define SUBTRACT_DC (0)
#define POOLED_COV  (1)

#define WRITE_IMAGES   (0)
#define PRINT_PROGRESS (0)
#define PRINT_BLOCKS   (0)
#define PRINT_COMP     (0)
#define PRINT_DROPS    (0)
#define PRINT_BETAS    (0)

typedef struct classify_ctx classify_ctx;

struct classify_ctx{
  int         n;
  intra_stats st;
  intra_stats gb;
  od_covmat   pd[OD_INTRA_NMODES];
  image_data  img;
#if WRITE_IMAGES
  image_files files;
#endif
  double      bits;
};

static void classify_ctx_init(classify_ctx *_this){
  int i;
  _this->n=0;
  intra_stats_init(&_this->st);
  intra_stats_init(&_this->gb);
  for(i=0;i<OD_INTRA_NMODES;i++){
    od_covmat_init(&_this->pd[i],5*B_SZ*B_SZ);
  }
}

static void classify_ctx_clear(classify_ctx *_this){
  int i;
  intra_stats_clear(&_this->st);
  intra_stats_clear(&_this->gb);
  for(i=0;i<OD_INTRA_NMODES;i++){
    od_covmat_clear(&_this->pd[i]);
  }
}

static void classify_ctx_reset(classify_ctx *_this){
  int i;
  _this->n=0;
  intra_stats_reset(&_this->gb);
  for(i=0;i<OD_INTRA_NMODES;i++){
    od_covmat_reset(&_this->pd[i]);
  }
  _this->bits=0;
}

static void classify_ctx_set_image(classify_ctx *_this,const char *_name,
 int _nxblocks,int _nyblocks){
  _this->n++;
  intra_stats_reset(&_this->st);
  image_data_init(&_this->img,_name,_nxblocks,_nyblocks);
#if WRITE_IMAGES
  image_files_init(&_this->files,_nxblocks,_nyblocks);
#endif
}

static void classify_ctx_clear_image(classify_ctx *_this){
  image_data_clear(&_this->img);
#if WRITE_IMAGES
  image_files_clear(&_this->files);
#endif
}

typedef struct prob_ctx prob_ctx;

struct prob_ctx{
        double *scale;
        double *xtx;
        double *xty;
  const double *mean;
  const double *cov;
        double *ete;
};

static void prob_ctx_init(prob_ctx *_this){
  _this->scale=(double *)malloc(sizeof(*_this->scale)*5*B_SZ*B_SZ);
  _this->xtx=(double *)malloc(sizeof(*_this->xtx)*4*B_SZ*B_SZ*4*B_SZ*B_SZ);
  _this->xty=(double *)malloc(sizeof(*_this->xty)*4*B_SZ*B_SZ*B_SZ*B_SZ);
  _this->mean=NULL;
  _this->cov=NULL;
  _this->ete=(double *)malloc(sizeof(*_this->ete)*B_SZ*B_SZ*B_SZ*B_SZ);
}

static void prob_ctx_clear(prob_ctx *_this){
  free(_this->scale);
  free(_this->xtx);
  free(_this->xty);
  _this->mean=NULL;
  _this->cov=NULL;
  free(_this->ete);
}

static void prob_ctx_load(prob_ctx *_this,od_covmat *_mat){
  int i;
  int j;
  /* compute the scale factors */
  for(i=0;i<5*B_SZ*B_SZ;i++){
    _this->scale[i]=sqrt(_mat->cov[i*5*B_SZ*B_SZ+i]);
  }
  /* normalize X^T*X and X^T*Y */
  for(j=0;j<4*B_SZ*B_SZ;j++){
    for(i=0;i<4*B_SZ*B_SZ;i++){
      _this->xtx[4*B_SZ*B_SZ*j+i]=
       _mat->cov[5*B_SZ*B_SZ*j+i]/(_this->scale[j]*_this->scale[i]);
    }
    for(i=0;i<B_SZ*B_SZ;i++){
      _this->xty[B_SZ*B_SZ*j+i]=_mat->cov[5*B_SZ*B_SZ*j+4*B_SZ*B_SZ+i];
      _this->xty[B_SZ*B_SZ*j+i]/=_this->scale[j]*_this->scale[4*B_SZ*B_SZ+i];
    }
  }
  _this->mean=_mat->mean;
  _this->cov=_mat->cov;
}

static void prob_ctx_comp_error(prob_ctx *_this,od_covmat *_mat,double *_beta_1){
  int j;
  int i;
  for(j=0;j<B_SZ*B_SZ;j++){
    for(i=0;i<B_SZ*B_SZ;i++){
      int ji;
      int l;
      int k;
      ji=B_SZ*B_SZ*j+i;
      l=5*B_SZ*B_SZ*(4*B_SZ*B_SZ+j);
      /* E^T*E = Y^T*Y - Y^T*X * beta_1 */
      _this->ete[ji]=_mat->cov[l+4*B_SZ*B_SZ+i];
      for(k=0;k<4*B_SZ*B_SZ;k++){
        _this->ete[ji]-=_mat->cov[l+k]*_beta_1[4*B_SZ*B_SZ*i+k];
      }
    }
  }
#if PRINT_COMP
  fprintf(stderr,"ete=[");
  for(j=0;j<B_SZ*B_SZ;j++){
    fprintf(stderr,"%s",j!=0?";":"");
    for(i=0;i<B_SZ*B_SZ;i++){
      fprintf(stderr,"%s%- 24.18G",i!=0?",":"",_this->ete[B_SZ*B_SZ*j+i]);
    }
  }
  fprintf(stderr,"];\n");
#endif
}

static void update_diversity(const double *_ete,double _b[B_SZ*B_SZ],
 const double *_scale){
  int v;
  int u;
  for(v=0;v<B_SZ;v++){
    for(u=0;u<B_SZ;u++){
      int i;
      int ii;
      i=B_SZ*v+u;
      ii=B_SZ*B_SZ*i+i;
      _b[i]=sqrt(_scale[v]*_scale[u]*_ete[ii]/2);
    }
  }
}

typedef struct solve_ctx solve_ctx;

struct solve_ctx{
#if USE_SVD
  double  *xtx;
  double **xtxp;
  double  *s;
#else
  double  *r;
  int     *pivot;
  double  *tau;
  double  *b;
  double  *work;
#endif
  double *beta_0;
  double *beta_1;
};

static void solve_ctx_init(solve_ctx *_this){
#if USE_SVD
  _this->xtx=(double *)malloc(sizeof(*_this->xtx)*2*4*B_SZ*B_SZ*4*B_SZ*B_SZ);
  _this->xtxp=(double **)malloc(sizeof(*_this->xtxp)*2*4*B_SZ*B_SZ);
  _this->s=(double *)malloc(sizeof(*_this->s)*4*B_SZ*B_SZ);
#else
  _this->r=(double *)malloc(sizeof(*_this->r)*UT_SZ(4*B_SZ*B_SZ,4*B_SZ*B_SZ));
  _this->pivot=(int *)malloc(sizeof(*_this->pivot)*4*B_SZ*B_SZ);
  _this->tau=(double *)malloc(sizeof(*_this->tau)*4*B_SZ*B_SZ);
  _this->b=(double *)malloc(sizeof(*_this->b)*4*B_SZ*B_SZ);
  _this->work=(double *)malloc(sizeof(*_this->work)*4*B_SZ*B_SZ);
#endif
  _this->beta_0=(double *)malloc(sizeof(*_this->beta_0)*B_SZ*B_SZ);
  _this->beta_1=(double *)malloc(sizeof(*_this->beta_1)*B_SZ*B_SZ*4*B_SZ*B_SZ);
}

static void solve_ctx_clear(solve_ctx *_this){
#if USE_SVD
  free(_this->xtx);
  free(_this->xtxp);
  free(_this->s);
#else
  free(_this->r);
  free(_this->pivot);
  free(_this->tau);
  free(_this->b);
  free(_this->work);
#endif
  free(_this->beta_0);
  free(_this->beta_1);
}

/* solve for beta_0[_y] and beta_1[_y] */
static void solve(const prob_ctx *_prob,solve_ctx *_sol,int _y,int *_mask,
 double *_beta_0,double *_beta_1){
  int nmi;
  int mi[4*B_SZ*B_SZ];
  int i;
  int j;
#if !USE_SVD
  int rank;
#endif
  nmi=0;
  for(i=0;i<4*B_SZ*B_SZ;i++){
    if(_mask[i]){
      mi[nmi]=i;
      nmi++;
    }
    _beta_1[_y*4*B_SZ*B_SZ+i]=0;
  }
#if USE_SVD
  for(j=0;j<nmi;j++){
    for(i=0;i<nmi;i++){
      _sol->xtx[4*B_SZ*B_SZ*j+i]=_prob->xtx[4*B_SZ*B_SZ*mi[j]+mi[i]];
    }
  }
  for(i=0;i<2*nmi;i++){
    _sol->xtxp[i]=&_sol->xtx[4*B_SZ*B_SZ*i];
  }
  svd_pseudoinverse(_sol->xtxp,_sol->s,nmi,nmi);
#else
  for(j=0;j<nmi;j++){
    for(i=j;i<nmi;i++){
      _sol->r[UT_IDX(j,i,nmi)]=_prob->xtx[4*B_SZ*B_SZ*mi[j]+mi[i]];
    }
    _sol->b[j]=_prob->xty[B_SZ*B_SZ*mi[j]+_y];
  }
  rank=cholesky(_sol->r,_sol->pivot,DBL_EPSILON,nmi);
  chdecomp(_sol->r,_sol->tau,rank,nmi);
  chsolve(_sol->r,_sol->pivot,_sol->tau,_sol->b,_sol->b,_sol->work,rank,nmi);
#endif
  /* compute beta_1 = (X^T*X)^-1 * X^T*Y and beta_0 = Ym - Xm * beta_1 */
  _beta_0[_y]=_prob->mean[4*B_SZ*B_SZ+_y];
  for(j=0;j<nmi;j++){
    int yj;
    yj=_y*4*B_SZ*B_SZ+mi[j];
#if USE_SVD
    _beta_1[yj]=0;
    for(i=0;i<nmi;i++){
      _beta_1[yj]+=_sol->xtx[4*B_SZ*B_SZ*j+i]*_prob->xty[B_SZ*B_SZ*mi[i]+_y];
    }
#else
    _beta_1[yj]=_sol->b[j];
#endif
    _beta_1[yj]*=_prob->scale[4*B_SZ*B_SZ+_y]/_prob->scale[mi[j]];
    _beta_0[_y]-=_prob->mean[mi[j]]*_beta_1[yj];
  }
}

static double comp_error(const prob_ctx *_prob,solve_ctx *_sol,int _y,
 int *_mask){
  double err;
  int    j;
  int    i;
  solve(_prob,_sol,_y,_mask,_sol->beta_0,_sol->beta_1);
  j=4*B_SZ*B_SZ+_y;
  err=_prob->cov[5*B_SZ*B_SZ*j+j];
  for(i=0;i<4*B_SZ*B_SZ;i++){
    if(_mask[i]){
      err-=_prob->cov[5*B_SZ*B_SZ*j+i]*_sol->beta_1[4*B_SZ*B_SZ*_y+i];
    }
  }
  return(err);
}

static int comp_delta_pg(const prob_ctx *_prob,solve_ctx _sol[NUM_PROCS],int _y,
 int _mask[4*B_SZ*B_SZ],double *_delta_pg){
  double s;
  int    i;
  int    j;
  int    nmi;
  int    mi[4*B_SZ*B_SZ];
  int    mask[NUM_PROCS][4*B_SZ*B_SZ];
  double delta_pg[4*B_SZ*B_SZ];
  nmi=0;
  for(i=0;i<4*B_SZ*B_SZ;i++){
    if(_mask[i]){
      mi[nmi]=i;
      nmi++;
    }
    for(j=0;j<NUM_PROCS;j++){
      mask[j][i]=_mask[i];
    }
  }
  s=1/comp_error(_prob,&_sol[0],_y,_mask);
  #pragma omp parallel for schedule(dynamic)
  for(i=0;i<nmi;i++){
    int tid;
    tid=omp_get_thread_num();
    mask[tid][mi[i]]=0;
    delta_pg[i]=comp_error(_prob,&_sol[tid],_y,mask[tid])*s;
    mask[tid][mi[i]]=1;
  }
#if SUBTRACT_DC
  if(_y==0) {
    for(i=0;i<nmi;i++){
      if(mi[i]%(B_SZ*B_SZ)==0){
        delta_pg[i]=UINT_MAX;
      }
    }
  }
#endif
  j=-1;
  for(i=0;i<nmi;i++){
    if(j==-1||delta_pg[i]<delta_pg[j]){
      j=i;
    }
  }
  if(j==-1){
    return j;
  }
  *_delta_pg=delta_pg[j];
  return(mi[j]);
}

static long timing(const struct timeb *_start,const struct timeb *_stop){
  long ms;
  ms=(_stop->time-_start->time)*1000;
  ms+=(_stop->millitm-_start->millitm);
  return ms;
}

static void comp_predictors(const prob_ctx *_prob,solve_ctx _sol[NUM_PROCS],
 int _drop,int _mask[B_SZ*B_SZ*4*B_SZ*B_SZ]){
  int i;
  int j;

#if PRINT_COMP
  fprintf(stderr,"xtx=[");
  for(j=0;j<4*B_SZ*B_SZ;j++){
    fprintf(stderr,"%s",j!=0?";":"");
    for(i=0;i<4*B_SZ*B_SZ;i++){
      fprintf(stderr,"%s%- 24.18G",i!=0?",":"",_prob->xtx[4*B_SZ*B_SZ*j+i]);
    }
  }
  fprintf(stderr,"];\n");

  fprintf(stderr,"xty=[");
  for(j=0;j<4*B_SZ*B_SZ;j++){
    fprintf(stderr,"%s",j!=0?";":"");
    for(i=0;i<B_SZ*B_SZ;i++){
      fprintf(stderr,"%s%- 24.18G",i!=0?",":"",_prob->xty[B_SZ*B_SZ*j+i]);
    }
  }
  fprintf(stderr,"];\n");
#endif

  if(_drop>0){
    double delta_pg[B_SZ*B_SZ];
    int    idx[B_SZ*B_SZ];
    for(j=0;j<B_SZ*B_SZ;j++){
      idx[j]=comp_delta_pg(_prob,_sol,j,&_mask[j*4*B_SZ*B_SZ],&delta_pg[j]);
    }
    while(_drop-->0){
      j=-1;
      for(i=0;i<B_SZ*B_SZ;i++){
        if(idx[i]!=-1&&(j==-1||delta_pg[i]<delta_pg[j])){
          j=i;
        }
      }
#if PRINT_DROPS
      printf("Dropping (%2i,%2i) cost Pg=%g\n",j,idx[j],10*log10(delta_pg[j]));
      fflush(stdout);
#endif
      _mask[j*4*B_SZ*B_SZ+idx[j]]=0;
      idx[j]=comp_delta_pg(_prob,_sol,j,&_mask[j*4*B_SZ*B_SZ],&delta_pg[j]);
    }
  }

  #pragma omp parallel for schedule(dynamic)
  for(j=0;j<B_SZ*B_SZ;j++){
    int tid;
    tid=omp_get_thread_num();
    solve(_prob,&_sol[tid],j,&_mask[j*4*B_SZ*B_SZ],_sol->beta_0,_sol->beta_1);
  }

#if PRINT_COMP
  fprintf(stderr,"beta_1=[");
  for(j=0;j<4*B_SZ*B_SZ;j++){
    fprintf(stderr,"%s",j!=0?";":"");
    for(i=0;i<B_SZ*B_SZ;i++){
      fprintf(stderr,"%s%- 24.18G",i!=0?",":"",_sol->beta_1[4*B_SZ*B_SZ*i+j]);
    }
  }
  fprintf(stderr,"];\n");

  fprintf(stderr,"beta_0=[");
  for(i=0;i<B_SZ*B_SZ;i++){
    fprintf(stderr,"%s%- 24.18G",i!=0?",":"",_sol->beta_0[i]);
  }
  fprintf(stderr,"];\n");
#endif
}

#if PRINT_PROGRESS
static void print_progress(FILE *_fp,const char *_proc){
  int tid;
  tid=omp_get_thread_num();
  fprintf(_fp,"thread %i in %s\n",tid,_proc);
}
#endif

static void ip_pre_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx *ctx;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress(stdout,"ip_pre_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  image_data_pre_block(&ctx->img,_data,_stride,_bi,_bj);
}

static void ip_fdct_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx *ctx;
  (void)_data;
  (void)_stride;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress(stdout,"ip_fdct_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  image_data_fdct_block(&ctx->img,_bi,_bj);
}

static void ip_add_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx *ctx;
  od_coeff     *block;
  int           by;
  int           bx;
  int           j;
  int           i;
  double        buf[5*B_SZ*B_SZ];
  int           mode;
  double        weight;
  (void)_data;
  (void)_stride;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress(stdout,"ip_add_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  for(by=0;by<=1;by++){
    for(bx=0;bx<=2-by;bx++){
      block=&ctx->img.fdct[ctx->img.fdct_stride*B_SZ*(_bj+by)+B_SZ*(_bi+bx)];
      for(j=0;j<B_SZ;j++){
        for(i=0;i<B_SZ;i++){
          buf[B_SZ*B_SZ*(3*by+bx)+j*B_SZ+i]=block[ctx->img.fdct_stride*j+i];
        }
      }
    }
  }
#if SUBTRACT_DC
  for(i=0;i<4;i++){
    buf[4*B_SZ*B_SZ]-=0.25*buf[i*B_SZ*B_SZ];
  }
#endif
  mode=ctx->img.mode[ctx->img.nxblocks*_bj+_bi];
  weight=ctx->img.weight[ctx->img.nxblocks*_bj+_bi];
  od_covmat_add(&ctx->pd[mode],buf,weight);
}

#if PRINT_BLOCKS
static void ip_print_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx *ctx;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress(stdout,"ip_print_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  image_data_print_block(&ctx->img,_bi,_bj,stderr);
}
#endif

static void ip_pred_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx *ctx;
  (void)_data;
  (void)_stride;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress("ip_pred_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  image_data_pred_block(&ctx->img,_bi,_bj);
}

static void ip_idct_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx *ctx;
  (void)_data;
  (void)_stride;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress("ip_idct_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  image_data_idct_block(&ctx->img,_bi,_bj);
}

static void ip_post_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx *ctx;
  (void)_data;
  (void)_stride;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress("ip_post_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  image_data_post_block(&ctx->img,_bi,_bj);
}

double b[OD_INTRA_NMODES][B_SZ*B_SZ];

static void ip_stats_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx *ctx;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress("ip_stats_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  image_data_stats_block(&ctx->img,_data,_stride,_bi,_bj,&ctx->st);
  {
    od_coeff *block;
    double   *pred;
    int       mode;
    int       j;
    int       i;
    block=&ctx->img.fdct[ctx->img.fdct_stride*B_SZ*(_bj+1)+B_SZ*(_bi+1)];
    pred=&ctx->img.pred[ctx->img.pred_stride*B_SZ*_bj+B_SZ*_bi];
    mode=ctx->img.mode[ctx->img.nxblocks*_bj+_bi];
    for(j=0;j<B_SZ;j++){
      for(i=0;i<B_SZ;i++){
        double res;
        res=sqrt(OD_SCALE[j]*OD_SCALE[i])*abs(block[ctx->img.fdct_stride*j+i]-(od_coeff)floor(pred[ctx->img.pred_stride*j+i]+0.5));
        ctx->bits+=1+OD_LOG2(b[mode][j*B_SZ+i])+M_LOG2E/b[mode][j*B_SZ+i]*res;
      }
    }
  }
}

#if WRITE_IMAGES
static void ip_files_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx *ctx;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress(stdout,"ip_files_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  image_data_files_block(&ctx->img,_data,_stride,_bi,_bj,&ctx->files);
}
#endif

int    step;

static void vp8_mode_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx  *ctx;
  unsigned char *mode;
  double        *weight;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress(stdout,"ip_vp8_mode_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  mode=&ctx->img.mode[ctx->img.nxblocks*_bj+_bi];
  weight=&ctx->img.weight[ctx->img.nxblocks*_bj+_bi];
  *mode=vp8_select_mode(_data,_stride,weight);
#if USE_WEIGHTS
  if(*mode==0){
    *weight=1;
  }
#else
  *weight=1;
#endif
}

static void od_mode_block(void *_ctx,const unsigned char *_data,int _stride,
 int _bi,int _bj){
  classify_ctx  *ctx;
  unsigned char *mode;
  od_coeff      *block;
  double        *weight;
  (void)_data;
  (void)_stride;
#if PRINT_PROGRESS
  if(_bi==0&&_bj==0){
    print_progress("od_mode_block");
  }
#endif
  ctx=(classify_ctx *)_ctx;
  mode=&ctx->img.mode[ctx->img.nxblocks*_bj+_bi];
  block=&ctx->img.fdct[ctx->img.fdct_stride*B_SZ*(_bj+1)+B_SZ*(_bi+1)];
  weight=&ctx->img.weight[ctx->img.nxblocks*_bj+_bi];
#if BITS_SELECT
  if(step==1){
    *mode=od_select_mode_satd(block,ctx->img.fdct_stride,weight);
  }
  else{
    *mode=od_select_mode_bits(block,ctx->img.fdct_stride,weight,b);
  }
#else
  *mode=od_select_mode_satd(block,ctx->img.fdct_stride,weight);
#endif
#if USE_WEIGHTS
  if(*mode==0){
    *weight=1;
  }
#else
  *weight=1;
#endif
}

static int init_start(void *_ctx,const char *_name,const th_info *_ti,int _pli,
 int _nxblocks,int _nyblocks){
  classify_ctx *ctx;
  (void)_ti;
  (void)_pli;
#if PRINT_PROGRESS
  print_progress(stdout,"init_start");
#endif
  fprintf(stdout,"%s\n",_name);
  fflush(stdout);
  ctx=(classify_ctx *)_ctx;
  classify_ctx_set_image(ctx,_name,_nxblocks,_nyblocks);
  return EXIT_SUCCESS;
}

static int init_finish(void *_ctx){
  classify_ctx *ctx;
#if PRINT_PROGRESS
  print_progress(stdout,"init_finish");
#endif
  ctx=(classify_ctx *)_ctx;
  /*intra_stats_combine(&ctx->gb,&ctx->st);
  intra_stats_correct(&ctx->st);
  fprintf(stdout,"%s\n",ctx->img.name);
  intra_stats_print(&ctx->st,"Daala Intra Predictors",OD_SCALE);
  fflush(stdout);*/
  image_data_save_map(&ctx->img);
  classify_ctx_clear_image(ctx);
  return EXIT_SUCCESS;
}

const block_func INIT[]={
  ip_pre_block,
  ip_fdct_block,
  vp8_mode_block,
  ip_add_block,
#if PRINT_BLOCKS
  ip_print_block,
#endif
};

const int NINIT=sizeof(INIT)/sizeof(*INIT);

static int pred_start(void *_ctx,const char *_name,const th_info *_ti,int _pli,
 int _nxblocks,int _nyblocks){
  classify_ctx *ctx;
  (void)_ti;
  (void)_pli;
#if PRINT_PROGRESS
  print_progress(stdout,"pred_start");
#endif
  ctx=(classify_ctx *)_ctx;
  classify_ctx_set_image(ctx,_name,_nxblocks,_nyblocks);
  image_data_load_map(&ctx->img);
  return EXIT_SUCCESS;
}

static int pred_finish(void *_ctx){
  classify_ctx *ctx;
#if WRITE_IMAGES
  char          suffix[16];
#endif
#if PRINT_PROGRESS
  print_progress(stdout,"pred_finish");
#endif
  ctx=(classify_ctx *)_ctx;
  intra_stats_combine(&ctx->gb,&ctx->st);
  intra_stats_correct(&ctx->st);
  fprintf(stdout,"%s\n",ctx->img.name);
  intra_stats_print(&ctx->st,"Daala Intra Predictors",OD_SCALE);
  fflush(stdout);
#if WRITE_IMAGES
  sprintf(suffix,"-step%02i",step);
  image_files_write(&ctx->files,ctx->img.name,suffix);
#endif
  image_data_save_map(&ctx->img);
  classify_ctx_clear_image(ctx);
  return EXIT_SUCCESS;
}

const block_func PRED[]={
  ip_pre_block,
  ip_fdct_block,
  od_mode_block,
  ip_add_block,
  ip_pred_block,
  ip_stats_block,
  ip_idct_block,
  ip_post_block,
#if WRITE_IMAGES
  ip_files_block,
#endif
};

const int NPRED=sizeof(PRED)/sizeof(*PRED);

#define PADDING (4*B_SZ)
#if PADDING<3*B_SZ
# error "PADDING must be at least 3*B_SZ"
#endif

#define INIT_STEPS (10)
#if B_SZ==4
# define DROP_STEPS (60)
# define DROPS_PER_STEP (16)
#elif B_SZ==8
# define DROP_STEPS (126)
# define DROPS_PER_STEP (128)
#elif B_SZ==16
# define DROP_STEPS (255)
# define DROPS_PER_STEP (1024)
#else
# error "Unsupported block size."
#endif

int main(int _argc,const char *_argv[]){
  classify_ctx   cls[NUM_PROCS];
  int            i;
  int            j;
  ne_filter_params_init();
  vp8_scale_init(VP8_SCALE);
  od_scale_init(OD_SCALE);
#if WRITE_IMAGES
  intra_map_colors(COLORS,OD_INTRA_NMODES);
#endif
  for(i=0;i<NUM_PROCS;i++){
    classify_ctx_init(&cls[i]);
  }
  omp_set_num_threads(NUM_PROCS);
  /* First pass across images uses VP8 mode selection. */
  ne_apply_to_blocks(cls,sizeof(*cls),0x1,PADDING,init_start,NINIT,INIT,
   init_finish,_argc,_argv);
  for(i=1;i<NUM_PROCS;i++){
    cls[0].n+=cls[i].n;
  }
  if(cls[0].n>0){
    prob_ctx      prob;
    solve_ctx     sol[NUM_PROCS];
    od_covmat     ete;
    int           mask[OD_INTRA_NMODES][B_SZ*B_SZ*4*B_SZ*B_SZ];
    struct timeb  start;
    struct timeb  stop;
    prob_ctx_init(&prob);
    for(i=0;i<NUM_PROCS;i++){
      solve_ctx_init(&sol[i]);
    }
    od_covmat_init(&ete,B_SZ*B_SZ);
    for(i=0;i<OD_INTRA_NMODES;i++){
      for(j=0;j<B_SZ*B_SZ*4*B_SZ*B_SZ;j++){
        mask[i][j]=1;
      }
    }
    ftime(&start);
    /* Each k-means step uses Daala mode selection. */
    for(step=1;step<=INIT_STEPS+DROP_STEPS;step++){
      int mults;
      int drops;
      mults=B_SZ*B_SZ*4*B_SZ*B_SZ;
      drops=0;
      if(step>INIT_STEPS){
        mults-=DROPS_PER_STEP*(step-INIT_STEPS);
        drops=DROPS_PER_STEP;
      }
      printf("Starting Step %02i (%i mults / block)\n",step,mults);
      for(j=0;j<OD_INTRA_NMODES;j++){
        /* Combine the gathered prediction data. */
        for(i=1;i<NUM_PROCS;i++){
          od_covmat_combine(&cls[0].pd[j],&cls[i].pd[j]);
        }
        prob_ctx_load(&prob,&cls[0].pd[j]);
        /* Update predictor model based on mults and drops. */
#if PRINT_DROPS
        if(drops>0){
          printf("Mode %i\n",j);
          fflush(stdout);
        }
#endif
        comp_predictors(&prob,sol,drops,mask[j]);
        /* Compute residual covariance for each mode. */
        prob_ctx_comp_error(&prob,&cls[0].pd[j],sol->beta_1);
#if ZERO_MEAN
        {
          double mean[B_SZ*B_SZ];
          for(i=0;i<B_SZ*B_SZ;i++){
            mean[i]=0;
          }
          od_covmat_update(&ete,prob.ete,mean,cls[0].pd[j].w);
        }
#else
        od_covmat_update(&ete,prob.ete,sol->beta_0,cls[0].pd[j].w);
#endif
#if !POOLED_COV
        od_covmat_correct(&ete);
        update_diversity(ete.cov,b[j],OD_SCALE);
        od_covmat_reset(&ete);
#endif
#if SUBTRACT_DC
        for(i=0;i<4;i++){
          OD_ASSERT(mask[j][i*B_SZ*B_SZ]);
          sol->beta_1[i*B_SZ*B_SZ]+=0.25;
        }
#endif
        update_predictors(j,sol->beta_0,sol->beta_1,mask[j]);
      }
#if POOLED_COV
      od_covmat_correct(&ete);
      for(j=0;j<OD_INTRA_NMODES;j++){
        update_diversity(ete.cov,b[j],OD_SCALE);
      }
      od_covmat_reset(&ete);
#endif
      /* Reset the prediction data. */
      for(i=0;i<NUM_PROCS;i++){
        classify_ctx_reset(&cls[i]);
      }
      /* Reclassify based on the new model. */
      ne_apply_to_blocks(cls,sizeof(*cls),0x1,PADDING,pred_start,NPRED,PRED,
       pred_finish,_argc,_argv);
      ftime(&stop);
      printf("Finished Step %02i (%lims)\n",step,timing(&start,&stop));
      start=stop;
      /* Combine the gathered intra stats. */
      for(i=1;i<NUM_PROCS;i++){
        intra_stats_combine(&cls[0].gb,&cls[i].gb);
        cls[0].bits+=cls[i].bits;
      }
      printf("Step %02i Total Bits %-24.18G\n",step,cls[0].bits);
      intra_stats_correct(&cls[0].gb);
      intra_stats_print(&cls[0].gb,"Daala Intra Predictors",OD_SCALE);
    }
    prob_ctx_clear(&prob);
    for(i=0;i<NUM_PROCS;i++){
      solve_ctx_clear(&sol[i]);
    }
    od_covmat_clear(&ete);
#if PRINT_BETAS
    print_predictors(stderr);
#endif
  }
  for(i=0;i<NUM_PROCS;i++){
    classify_ctx_clear(&cls[i]);
  }
  return EXIT_SUCCESS;
}
