#include "accel.h"
#include "scs.h"
#include "scs_blas.h"

/* Not clear if this should just be 0. */
#define ACCEL_REGULARIZATION (0.)

struct SCS_ACCEL {
#ifdef LAPACK_LIB_FOUND
  scs_float *dF;
  scs_float *dG;
  scs_float *f;
  scs_float *g;
  scs_float *theta;
  scs_float *tmp;
  scs_float *X;
  scs_float *dFQR;
  scs_float *wrk;
  scs_int k, l, bad_numerics;
  blasint worksize;
#endif
  scs_float totalAccelTime;
};

#ifdef LAPACK_LIB_FOUND
void BLAS(gemv)(const char *trans, const blasint *m, const blasint *n,
                const scs_float *alpha, const scs_float *a, const blasint *lda,
                const scs_float *x, const blasint *incx, const scs_float *beta,
                scs_float *y, const blasint *incy);
void BLAS(syrk)(const char *uplo, const char *trans, blasint *n, blasint *k,
                scs_float *alpha, scs_float *a, blasint *lda, scs_float *beta,
                scs_float *c, blasint *ldc);
void BLAS(posv) (const char *uplo, blasint * n, blasint * nrhs, scs_float * a,
                 blasint * lda, scs_float * b, blasint * ldb, blasint * info);
void BLAS(gels)(const char *trans, const blasint *m, const blasint *n,
                const blasint *nrhs, scs_float *a, const blasint *lda, scs_float *b,
                const blasint *ldb, scs_float *work, const blasint *lwork, blasint *info);

scs_int solve_with_gels(Accel * a) {
  DEBUG_FUNC
  blasint info;
  scs_int l = a->l;
  blasint twol = 2 * l;
  blasint one = 1;
  blasint k = (blasint) a->k;
  scs_float negOnef = -1.0;
  scs_float onef = 1.0;
  memcpy(a->theta, a->f, sizeof(scs_float) * 2 * l);
  memcpy(a->dFQR, a->dF, sizeof(scs_float) * 2 * l * k);
  BLAS(gels)("NoTrans", &twol, &k, &one, a->dFQR, &twol, a->theta, &twol, a->wrk, &(a->worksize), &info);
  memcpy(a->tmp, a->g, sizeof(scs_float) * 2 * l);
  /* matmul g = g - dG * theta */
  BLAS(gemv)("NoTrans", &twol, &k, &negOnef, a->dG, &twol, a->theta, &one, &onef, a->tmp, &one);
  RETURN info;
}

blasint init_accel_work(Accel * a){
  DEBUG_FUNC
  blasint info;
  scs_float twork;
  scs_int l = a->l;
  blasint lwork = -1;
  blasint twol = 2 * l;
  blasint one = 1;
  blasint k = (blasint) a->k;
  blasint worksize;
  BLAS(gels)("NoTrans", &twol, &k, &one, a->dFQR, &twol, a->theta, &twol, &twork, &lwork, &info);
  worksize = (blasint) twork;
  a->worksize = worksize;
  a->dFQR = scs_malloc(sizeof(scs_float) * 2 * l * a->k);
  a->wrk = scs_malloc(sizeof(blasint) * worksize);
  RETURN info;
}

scs_int solve_accel_linsys(Accel *a) {
  DEBUG_FUNC
  if (a->bad_numerics) RETURN solve_with_gels(a);
  blasint twol = 2 * a->l;
  blasint one = 1;
  blasint k = (blasint)a->k;
  scs_float negOnef = -1.0;
  scs_float onef = 1.0;
  scs_float zerof = 0.0;
  scs_int info, i;
  memset(a->X, 0, a->k * a->k * sizeof(scs_float));
  if (ACCEL_REGULARIZATION > 0.) {
    for (i = 0; i < a->k; ++i) {
      a->X[i * a->k + i] = ACCEL_REGULARIZATION;
    }
  }
  /* X = dF'*dF */
  BLAS(syrk)("Lower", "Transpose", &k, &twol, &onef, a->dF, &twol, &onef, a->X, &k);
  /* theta = dF' f */
  BLAS(gemv)("Trans", &twol, &k, &onef, a->dF, &twol, a->f, &one, &zerof, a->theta, &one);
  /* theta = (dF'dF) \ dF' f */
  BLAS(posv)("Lower", &k, &one, a->X, &k, a->theta, &k, &info);
  if (info != 0) {
    scs_printf("\tposv ran into numerical issues, info %i, switching to gels\n", info);
    a->bad_numerics = 1;
    init_accel_work(a);
    RETURN solve_with_gels(a);
  }
  memcpy(a->tmp, a->g, sizeof(scs_float) * 2 * a->l);
  /* g = g - dG * theta */
  BLAS(gemv)("NoTrans", &twol, &k, &negOnef, a->dG, &twol, a->theta, &one, &onef, a->tmp, &one);
  RETURN info;
}

void update_accel_params(Work *w, scs_int idx) {
  DEBUG_FUNC
  scs_float *dF = w->accel->dF;
  scs_float *dG = w->accel->dG;
  scs_float *f = w->accel->f;
  scs_float *g = w->accel->g;
  scs_int l = w->m + w->n + 1;
  /* copy g_prev into idx col of dG */
  memcpy(&(dG[idx * 2 * l]), g, sizeof(scs_float) * 2 * l);
  /* copy f_prev into idx col of dF */
  memcpy(&(dF[idx * 2 * l]), f, sizeof(scs_float) * 2 * l);
  /* g = [u;v] */
  memcpy(g, w->u, sizeof(scs_float) * l);
  memcpy(&(g[l]), w->v, sizeof(scs_float) * l);
  /* calculcate f = g - [u_prev, v_prev] */
  memcpy(f, g, sizeof(scs_float) * 2 * l);
  addScaledArray(f, w->u_prev, l, -1.0);
  addScaledArray(&(f[l]), w->v_prev, l, -1.0);
  /* idx col of dG = g_prev - g */
  addScaledArray(&(dG[idx * 2 * l]), g, 2 * l, -1);
  /* idx col of dF = f_prev - f */
  addScaledArray(&(dF[idx * 2 * l]), f, 2 * l, -1);
  RETURN;
}

Accel *initAccel(Work *w) {
  DEBUG_FUNC
  Accel *a = scs_malloc(sizeof(Accel));
  scs_int l = w->m + w->n + 1;
  if (!a) {
    RETURN SCS_NULL;
  }
  a->bad_numerics = 0;
  a->l = l;
  /* k = lookback - 1 since we use the difference form
     of anderson acceleration, and so there is one fewer var in lin sys. */
  /* Use MIN to prevent not full rank matrices */
  a->k = MIN(2 * l, w->stgs->acceleration_lookback - 1);
  if (a->k <= 0) {
    a->dF = a->dG = a->f = a->g = a->theta = a->tmp = SCS_NULL;
    RETURN a;
  }
  a->dF = scs_malloc(sizeof(scs_float) * 2 * l * a->k);
  a->dG = scs_malloc(sizeof(scs_float) * 2 * l * a->k);
  a->f = scs_malloc(sizeof(scs_float) * 2 * l);
  a->g = scs_malloc(sizeof(scs_float) * 2 * l);
  a->theta = scs_malloc(sizeof(scs_float) * MAX(2 * l, a->k));
  a->tmp = scs_malloc(sizeof(scs_float) * 2 * l);
  a->X = scs_malloc(sizeof(scs_float) * a->k * a->k);
  a->wrk = a->dFQR = SCS_NULL;
  a->totalAccelTime = 0.0;
  if (!a->dF || !a->dG || !a->f || !a->g || !a->theta || !a->tmp || !a->X) {
    freeAccel(a);
    a = SCS_NULL;
  }
  RETURN a;
}

scs_int accelerate(Work *w, scs_int iter) {
  DEBUG_FUNC
  scs_float *tmp = w->accel->tmp;
  scs_int l = w->accel->l;
  scs_int k = w->accel->k;
  scs_int info;
  timer accelTimer;
  tic(&accelTimer);
  if (k <= 0) {
    RETURN 0;
  }
  /* update dF, dG, f, g */
  update_accel_params(w, (k + iter - 1) % k);
  if (iter < k) {
    RETURN 0;
  }
  /* solve linear system for theta */
  info = solve_accel_linsys(w->accel);
  /* set [u;v] = tmp */
  memcpy(w->u, tmp, sizeof(scs_float) * l);
  memcpy(w->v, &(tmp[l]), sizeof(scs_float) * l);
  w->accel->totalAccelTime += tocq(&accelTimer);
  if (info != 0) {
    scs_printf("Accelerate error, info %i\n", info);
  }
  RETURN info;
}

void freeAccel(Accel *a) {
  DEBUG_FUNC
  if (a) {
    if (a->dF) scs_free(a->dF);
    if (a->dG) scs_free(a->dG);
    if (a->f) scs_free(a->f);
    if (a->g) scs_free(a->g);
    if (a->theta) scs_free(a->theta);
    if (a->tmp) scs_free(a->tmp);
    if (a->X) scs_free(a->X);
    if (a->dFQR) scs_free(a->dFQR);
    if (a->wrk) scs_free(a->wrk);
    scs_free(a);
  }
  RETURN;
}

#else

Accel *initAccel(Work *w) {
  Accel *a = scs_malloc(sizeof(Accel));
  a->totalAccelTime = 0.0;
  RETURN a;
}

void freeAccel(Accel *a) {
  if (a) scs_free(a);
}

scs_int accelerate(Work *w, scs_int iter) { RETURN 0; }
#endif

char *getAccelSummary(const Info *info, Accel *a) {
  DEBUG_FUNC
  char *str = scs_malloc(sizeof(char) * 64);
  sprintf(str, "\tAcceleration: avg solve time: %1.2es\n",
          a->totalAccelTime / (info->iter + 1) / 1e3);
  a->totalAccelTime = 0.0;
  RETURN str;
}
