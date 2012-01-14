/*$Id$*/

/*
  Include "tao.h" so we can use TAO solvers with PETSc support.  
  Include "petscda.h" so that we can use distributed arrays (DAs) for managing
  the parallel mesh.
*/

#include "petscda.h"
#include "tao.h"
#include <math.h>  /* for cos() sin(0), and atan() */

static  char help[]=
"This example demonstrates use of the TAO package to \n\
solve a bound constrained minimization problem.  This example is based on \n\
the problem DPJB from the MINPACK-2 test suite.  This pressure journal \n\
bearing problem is an example of elliptic variational problem defined over \n\
a two dimensional rectangle.  By discretizing the domain into triangular \n\
elements, the pressure surrounding the journal bearing is defined as the \n\
minimum of a quadratic function whose variables are bounded below by zero.\n\
The command line options are:\n\
  -mx <xg>, where <xg> = number of grid points in the 1st coordinate direction\n\
  -my <yg>, where <yg> = number of grid points in the 2nd coordinate direction\n\
 \n";

/*T
   Concepts: TAO - Solving a bound constrained minimization problem
   Routines: TaoInitialize(); TaoFinalize();
   Routines: TaoApplicationCreate();  TaoAppDestroy();
   Routines: TaoAppSetObjectiveAndGradientRoutine(); TaoAppSetHessianRoutine();
   Routines: TaoAppSetInitialSolutionVec(); TaoAppSetHessianMat();
   Routines: TaoCreate(); TaoDestroy();
   Routines: TaoSetOptions(); TaoApplyGetKSP()
   Routines: TaoSolveApplication(); TaoGetTerminationReason();
   Processors: n
T*/

/* 
   User-defined application context - contains data needed by the 
   application-provided call-back routines, FormFunctionGradient(),
   FormHessian().
*/
typedef struct {
  /* problem parameters */
  PetscReal      ecc;          /* test problem parameter */
  PetscReal      b;            /* A dimension of journal bearing */
  PetscInt       nx,ny;        /* discretization in x, y directions */

  /* Working space */
  DA          da;           /* distributed array data structure */
  Mat         A;            /* Quadratic Objective term */
  Vec         B;            /* Linear Objective term */
} AppCtx;

/* User-defined routines */
static PetscReal p(PetscReal xi, PetscReal ecc);
static int FormFunctionGradient(TAO_APPLICATION, Vec, double *,Vec,void *);
static int FormHessian(TAO_APPLICATION,Vec,Mat *, Mat *, MatStructure *, void *);
static int ComputeB(AppCtx*);
static int ComputeBounds(TAO_APPLICATION, Vec, Vec, void *);

#undef __FUNCT__
#define __FUNCT__ "main"
int main( int argc, char **argv )
{
  int        info;               /* used to check for functions returning nonzeros */
  PetscInt   Nx, Ny;             /* number of processors in x- and y- directions */
  PetscInt   m, N;               /* number of local and global elements in vectors */
  Vec        x;                  /* variables vector */
  PetscTruth   flg;              /* A return variable when checking for user options */
  TAO_SOLVER tao;                /* TAO_SOLVER solver context */
  TaoMethod  method = "tao_gpcg";/* default minimization method */
  TAO_APPLICATION jbearingapp;   /* The PETSc application */
  ISLocalToGlobalMapping isltog; /* local-to-global mapping object */
  PetscInt  nloc;                      /* The number of local elements */
  PetscInt *ltog;                     /* mapping of local elements to global elements */
  TaoTerminateReason reason;
  AppCtx     user;               /* user-defined work context */
  PetscReal     zero=0.0;           /* lower bound on all variables */
  KSP      ksp;

  
  /* Initialize PETSC and TAO */
  PetscInitialize( &argc, &argv,(char *)0,help );
  TaoInitialize( &argc, &argv,(char *)0,help );

  /* Set the default values for the problem parameters */
  user.nx = 50; user.ny = 50; user.ecc = 0.1; user.b = 10.0;

  /* Check for any command line arguments that override defaults */
  info = PetscOptionsGetInt(TAO_NULL,"-mx",&user.nx,&flg); CHKERRQ(info);
  info = PetscOptionsGetInt(TAO_NULL,"-my",&user.ny,&flg); CHKERRQ(info);
  info = PetscOptionsGetReal(TAO_NULL,"-ecc",&user.ecc,&flg); CHKERRQ(info);
  info = PetscOptionsGetReal(TAO_NULL,"-b",&user.b,&flg); CHKERRQ(info);


  PetscPrintf(MPI_COMM_WORLD,"\n---- Journal Bearing Problem -----\n");
  PetscPrintf(MPI_COMM_WORLD,"mx: %d,  my: %d,  ecc: %4.3f \n\n",
	      user.nx,user.ny,user.ecc);

  /* Calculate any derived values from parameters */
  N = user.nx*user.ny; 

  /* Let Petsc determine the grid division */
  Nx = PETSC_DECIDE; Ny = PETSC_DECIDE;

  /*
     A two dimensional distributed array will help define this problem,
     which derives from an elliptic PDE on two dimensional domain.  From
     the distributed array, Create the vectors.
  */
  info = DACreate2d(MPI_COMM_WORLD,DA_NONPERIODIC,DA_STENCIL_STAR,user.nx,
                    user.ny,Nx,Ny,1,1,PETSC_NULL,PETSC_NULL,&user.da); CHKERRQ(info);

  /*
     Extract global and local vectors from DA; the vector user.B is
     used solely as work space for the evaluation of the function, 
     gradient, and Hessian.  Duplicate for remaining vectors that are 
     the same types.
  */
  info = DACreateGlobalVector(user.da,&x); CHKERRQ(info); /* Solution */
  info = VecDuplicate(x,&user.B); CHKERRQ(info); /* Linear objective */


  /*  Create matrix user.A to store quadratic, Create a local ordering scheme. */
  info = VecGetLocalSize(x,&m); CHKERRQ(info);
  info = MatCreateMPIAIJ(MPI_COMM_WORLD,m,m,N,N,5,TAO_NULL,3,TAO_NULL,&user.A); CHKERRQ(info);
  
  info = DAGetGlobalIndices(user.da,&nloc,&ltog); CHKERRQ(info);
  info = ISLocalToGlobalMappingCreate(MPI_COMM_SELF,nloc,ltog,&isltog); 
  CHKERRQ(info);
  info = MatSetLocalToGlobalMapping(user.A,isltog); CHKERRQ(info);
  info = ISLocalToGlobalMappingDestroy(isltog); CHKERRQ(info);


  /* User defined function -- compute linear term of quadratic */
  info = ComputeB(&user); CHKERRQ(info);


  /* The TAO code begins here */

  /* 
     Create the optimization solver, Petsc application 
     Suitable methods: "tao_gpcg","tao_bqpip","tao_tron","tao_blmvm" 
  */
  info = TaoCreate(MPI_COMM_WORLD,method,&tao); CHKERRQ(info);
  info = TaoApplicationCreate(MPI_COMM_WORLD,&jbearingapp); CHKERRQ(info);

  /* Set the initial vector */
  info = VecSet(x, zero); CHKERRQ(info);
  info = TaoAppSetInitialSolutionVec(jbearingapp,x); CHKERRQ(info);

  /* Set the user function, gradient, hessian evaluation routines and data structures */
  info = TaoAppSetObjectiveAndGradientRoutine(jbearingapp,FormFunctionGradient,(void*) &user); 
  CHKERRQ(info);
  
  info = TaoAppSetHessianMat(jbearingapp,user.A,user.A); CHKERRQ(info);
  info = TaoAppSetHessianRoutine(jbearingapp,FormHessian,(void*)&user); 
  CHKERRQ(info);

  /* Set a routine that defines the bounds */
  info = TaoAppSetVariableBoundsRoutine(jbearingapp,ComputeBounds,(void*)&user); CHKERRQ(info);

  info = TaoAppGetKSP(jbearingapp,&ksp); CHKERRQ(info);
  if (ksp) {                                         
    info = KSPSetType(ksp,KSPCG); CHKERRQ(info);
  }



  /* Check for any tao command line options */
  info = TaoSetOptions(jbearingapp,tao); CHKERRQ(info);

  /* Solve the bound constrained problem */
  info = TaoSolveApplication(jbearingapp,tao); CHKERRQ(info);

  info = TaoGetTerminationReason(tao,&reason); CHKERRQ(info);
  if (reason <= 0)
    PetscPrintf(MPI_COMM_WORLD,"Try a different TAO method, adjust some parameters, or check the function evaluation routines\n");

  /* Free TAO data structures */
  info = TaoDestroy(tao); CHKERRQ(info);
  info = TaoAppDestroy(jbearingapp); CHKERRQ(info);

  /* Free PETSc data structures */
  info = VecDestroy(x); CHKERRQ(info); 
  info = MatDestroy(user.A); CHKERRQ(info);
  info = VecDestroy(user.B); CHKERRQ(info); 
  info = DADestroy(user.da); CHKERRQ(info);

  TaoFinalize();
  PetscFinalize();

  return 0;
}

#undef __FUNCT__
#define __FUNCT__ "ComputeBounds"
static int ComputeBounds(TAO_APPLICATION taoapp, Vec xl, Vec xu, void *ctx){
  int info;
  PetscReal zero=0.0, d1000=1000;
  /* Set the upper and lower bounds */
  info = VecSet(xl, zero); CHKERRQ(info);
  info = VecSet(xu, d1000); CHKERRQ(info);
  return 0;
}

static PetscReal p(PetscReal xi, PetscReal ecc)
{ 
  PetscReal t=1.0+ecc*cos(xi); 
  return (t*t*t); 
}

#undef __FUNCT__
#define __FUNCT__ "ComputeB"
int ComputeB(AppCtx* user)
{
  int info;
  PetscInt i,j,k;
  PetscInt nx,ny,xs,xm,gxs,gxm,ys,ym,gys,gym;
  PetscReal two=2.0, pi=4.0*atan(1.0);
  PetscReal hx,hy,ehxhy;
  PetscReal temp,*b;
  PetscReal ecc=user->ecc;

  nx=user->nx;
  ny=user->ny;
  hx=two*pi/(nx+1.0);
  hy=two*user->b/(ny+1.0);
  ehxhy = ecc*hx*hy;


  /*
     Get local grid boundaries
  */
  info = DAGetCorners(user->da,&xs,&ys,TAO_NULL,&xm,&ym,TAO_NULL); CHKERRQ(info);
  info = DAGetGhostCorners(user->da,&gxs,&gys,TAO_NULL,&gxm,&gym,TAO_NULL); CHKERRQ(info);
  

  /* Compute the linear term in the objective function */  
  info = VecGetArray(user->B,&b); CHKERRQ(info);
  for (i=xs; i<xs+xm; i++){
    temp=sin((i+1)*hx);
    for (j=ys; j<ys+ym; j++){
      k=xm*(j-ys)+(i-xs);
      b[k]=  - ehxhy*temp;
    }
  }
  info = VecRestoreArray(user->B,&b); CHKERRQ(info);
  info = PetscLogFlops(5*xm*ym+3*xm); CHKERRQ(info);

  return 0;
}

#undef __FUNCT__
#define __FUNCT__ "FormFunctionGradient"
int FormFunctionGradient(TAO_APPLICATION taoapp, Vec X, double *fcn,Vec G,void *ptr)
{
  AppCtx* user=(AppCtx*)ptr;
  int info;
  PetscInt i,j,k,kk;
  PetscInt col[5],row,nx,ny,xs,xm,gxs,gxm,ys,ym,gys,gym;
  PetscReal one=1.0, two=2.0, six=6.0,pi=4.0*atan(1.0);
  PetscReal hx,hy,hxhy,hxhx,hyhy;
  PetscReal xi,v[5];
  PetscReal ecc=user->ecc, trule1,trule2,trule3,trule4,trule5,trule6;
  PetscReal vmiddle, vup, vdown, vleft, vright;
  PetscReal tt,f1,f2;
  PetscReal *x,*g,zero=0.0;
  Vec localX;

  nx=user->nx;
  ny=user->ny;
  hx=two*pi/(nx+1.0);
  hy=two*user->b/(ny+1.0);
  hxhy=hx*hy;
  hxhx=one/(hx*hx);
  hyhy=one/(hy*hy);

  info = DAGetLocalVector(user->da,&localX);CHKERRQ(info);

  info = DAGlobalToLocalBegin(user->da,X,INSERT_VALUES,localX); CHKERRQ(info);
  info = DAGlobalToLocalEnd(user->da,X,INSERT_VALUES,localX); CHKERRQ(info);

  info = VecSet(G, zero); CHKERRQ(info);
  /*
    Get local grid boundaries
  */
  info = DAGetCorners(user->da,&xs,&ys,TAO_NULL,&xm,&ym,TAO_NULL); CHKERRQ(info);
  info = DAGetGhostCorners(user->da,&gxs,&gys,TAO_NULL,&gxm,&gym,TAO_NULL); CHKERRQ(info);
  
  info = VecGetArray(localX,&x); CHKERRQ(info);
  info = VecGetArray(G,&g); CHKERRQ(info);

  for (i=xs; i< xs+xm; i++){
    xi=(i+1)*hx;
    trule1=hxhy*( p(xi,ecc) + p(xi+hx,ecc) + p(xi,ecc) ) / six; /* L(i,j) */
    trule2=hxhy*( p(xi,ecc) + p(xi-hx,ecc) + p(xi,ecc) ) / six; /* U(i,j) */
    trule3=hxhy*( p(xi,ecc) + p(xi+hx,ecc) + p(xi+hx,ecc) ) / six; /* U(i+1,j) */
    trule4=hxhy*( p(xi,ecc) + p(xi-hx,ecc) + p(xi-hx,ecc) ) / six; /* L(i-1,j) */
    trule5=trule1; /* L(i,j-1) */
    trule6=trule2; /* U(i,j+1) */

    vdown=-(trule5+trule2)*hyhy;
    vleft=-hxhx*(trule2+trule4);
    vright= -hxhx*(trule1+trule3);
    vup=-hyhy*(trule1+trule6);
    vmiddle=(hxhx)*(trule1+trule2+trule3+trule4)+hyhy*(trule1+trule2+trule5+trule6);

    for (j=ys; j<ys+ym; j++){
      
      row=(j-gys)*gxm + (i-gxs);
       v[0]=0; v[1]=0; v[2]=0; v[3]=0; v[4]=0;
       
       k=0;
       if (j>gys){ 
	 v[k]=vdown; col[k]=row - gxm; k++;
       }
       
       if (i>gxs){
	 v[k]= vleft; col[k]=row - 1; k++;
       }

       v[k]= vmiddle; col[k]=row; k++;
       
       if (i+1 < gxs+gxm){
	 v[k]= vright; col[k]=row+1; k++;
       }
       
       if (j+1 <gys+gym){
	 v[k]= vup; col[k] = row+gxm; k++;
       }
       tt=0;
       for (kk=0;kk<k;kk++){
	 tt+=v[kk]*x[col[kk]];
       }
       row=(j-ys)*xm + (i-xs);
       g[row]=tt;

     }

  }

  info = VecRestoreArray(localX,&x); CHKERRQ(info);
  info = VecRestoreArray(G,&g); CHKERRQ(info);

  info = DARestoreLocalVector(user->da,&localX); CHKERRQ(info);

  info = VecDot(X,G,&f1); CHKERRQ(info);
  info = VecDot(user->B,X,&f2); CHKERRQ(info);
  info = VecAXPY(G, one, user->B); CHKERRQ(info);
  *fcn = f1/2.0 + f2;

  info = PetscLogFlops((91 + 10*ym) * xm); CHKERRQ(info);
  return 0;

}



#undef __FUNCT__
#define __FUNCT__ "FormHessian"
/* 
   FormHessian computes the quadratic term in the quadratic objective function 
   Notice that the objective function in this problem is quadratic (therefore a constant
   hessian).  If using a nonquadratic solver, then you might want to reconsider this function
*/
int FormHessian(TAO_APPLICATION taoapp,Vec X,Mat *H, Mat *Hpre, MatStructure *flg, void *ptr)
{
  AppCtx* user=(AppCtx*)ptr;
  int info;
  PetscInt i,j,k;
  PetscInt col[5],row,nx,ny,xs,xm,gxs,gxm,ys,ym,gys,gym;
  PetscReal one=1.0, two=2.0, six=6.0,pi=4.0*atan(1.0);
  PetscReal hx,hy,hxhy,hxhx,hyhy;
  PetscReal xi,v[5];
  PetscReal ecc=user->ecc, trule1,trule2,trule3,trule4,trule5,trule6;
  PetscReal vmiddle, vup, vdown, vleft, vright;
  Mat hes=*H;
  PetscTruth assembled;

  nx=user->nx;
  ny=user->ny;
  hx=two*pi/(nx+1.0);
  hy=two*user->b/(ny+1.0);
  hxhy=hx*hy;
  hxhx=one/(hx*hx);
  hyhy=one/(hy*hy);

  *flg=SAME_NONZERO_PATTERN;
  /*
    Get local grid boundaries
  */
  info = DAGetCorners(user->da,&xs,&ys,TAO_NULL,&xm,&ym,TAO_NULL); CHKERRQ(info);
  info = DAGetGhostCorners(user->da,&gxs,&gys,TAO_NULL,&gxm,&gym,TAO_NULL); CHKERRQ(info);
  
  info = MatAssembled(hes,&assembled); CHKERRQ(info);
  if (assembled){info = MatZeroEntries(hes);  CHKERRQ(info);}

  for (i=xs; i< xs+xm; i++){
    xi=(i+1)*hx;
    trule1=hxhy*( p(xi,ecc) + p(xi+hx,ecc) + p(xi,ecc) ) / six; /* L(i,j) */
    trule2=hxhy*( p(xi,ecc) + p(xi-hx,ecc) + p(xi,ecc) ) / six; /* U(i,j) */
    trule3=hxhy*( p(xi,ecc) + p(xi+hx,ecc) + p(xi+hx,ecc) ) / six; /* U(i+1,j) */
    trule4=hxhy*( p(xi,ecc) + p(xi-hx,ecc) + p(xi-hx,ecc) ) / six; /* L(i-1,j) */
    trule5=trule1; /* L(i,j-1) */
    trule6=trule2; /* U(i,j+1) */

    vdown=-(trule5+trule2)*hyhy;
    vleft=-hxhx*(trule2+trule4);
    vright= -hxhx*(trule1+trule3);
    vup=-hyhy*(trule1+trule6);
    vmiddle=(hxhx)*(trule1+trule2+trule3+trule4)+hyhy*(trule1+trule2+trule5+trule6);
    v[0]=0; v[1]=0; v[2]=0; v[3]=0; v[4]=0;

    for (j=ys; j<ys+ym; j++){
      row=(j-gys)*gxm + (i-gxs);
       
      k=0;
      if (j>gys){ 
	v[k]=vdown; col[k]=row - gxm; k++;
      }
       
      if (i>gxs){
	v[k]= vleft; col[k]=row - 1; k++;
      }

      v[k]= vmiddle; col[k]=row; k++;
       
      if (i+1 < gxs+gxm){
	v[k]= vright; col[k]=row+1; k++;
      }
       
      if (j+1 <gys+gym){
	v[k]= vup; col[k] = row+gxm; k++;
      }
      info = MatSetValuesLocal(hes,1,&row,k,col,v,INSERT_VALUES); CHKERRQ(info);
       
    }

  }

  /* 
     Assemble matrix, using the 2-step process:
     MatAssemblyBegin(), MatAssemblyEnd().
     By placing code between these two statements, computations can be
     done while messages are in transition.
  */
  info = MatAssemblyBegin(hes,MAT_FINAL_ASSEMBLY); CHKERRQ(info);
  info = MatAssemblyEnd(hes,MAT_FINAL_ASSEMBLY); CHKERRQ(info);

  /*
    Tell the matrix we will never add a new nonzero location to the
    matrix. If we do it will generate an error.
  */
  info = MatSetOption(hes,MAT_NEW_NONZERO_LOCATION_ERR,PETSC_TRUE); CHKERRQ(info);
  info = MatSetOption(hes,MAT_SYMMETRIC,PETSC_TRUE); CHKERRQ(info);

  info = PetscLogFlops(9*xm*ym+49*xm); CHKERRQ(info);

  return 0;
}
