// Copyright (c) 2010-2022, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "lor_batched.hpp"
#include "../../fem/quadinterpolator.hpp"
#include "../../general/forall.hpp"
#include <climits>

// Specializations
#include "lor_h1.hpp"
#include "lor_nd.hpp"
#include "lor_rt.hpp"

namespace mfem
{

template <typename T1, typename T2>
bool HasIntegrators(BilinearForm &a)
{
   Array<BilinearFormIntegrator*> *integs = a.GetDBFI();
   if (integs == NULL) { return false; }
   if (integs->Size() == 1)
   {
      BilinearFormIntegrator *i0 = (*integs)[0];
      if (dynamic_cast<T1*>(i0) || dynamic_cast<T2*>(i0)) { return true; }
   }
   else if (integs->Size() == 2)
   {
      BilinearFormIntegrator *i0 = (*integs)[0];
      BilinearFormIntegrator *i1 = (*integs)[1];
      if ((dynamic_cast<T1*>(i0) && dynamic_cast<T2*>(i1)) ||
          (dynamic_cast<T2*>(i0) && dynamic_cast<T1*>(i1)))
      {
         return true;
      }
   }
   return false;
}

#ifdef MFEM_USE_MPI

void HypreStealOwnership(HypreParMatrix &A_hyp, SparseMatrix &A_diag)
{
#ifndef HYPRE_BIGINT
   bool own_i = A_hyp.GetDiagMemoryI().OwnsHostPtr();
   bool own_j = A_hyp.GetDiagMemoryJ().OwnsHostPtr();
   MFEM_ASSERT(own_i == own_j, "Inconsistent ownership");
   if (!own_i)
   {
      std::swap(A_diag.GetMemoryI(), A_hyp.GetDiagMemoryI());
      std::swap(A_diag.GetMemoryJ(), A_hyp.GetDiagMemoryJ());
   }
#endif
   if (!A_hyp.GetDiagMemoryData().OwnsHostPtr())
   {
      std::swap(A_diag.GetMemoryData(), A_hyp.GetDiagMemoryData());
   }
   A_hyp.SetOwnerFlags(3, A_hyp.OwnsOffd(), A_hyp.OwnsColMap());
}

#endif

bool BatchedLORAssembly::FormIsSupported(BilinearForm &a)
{
   const FiniteElementCollection *fec = a.FESpace()->FEColl();
   // TODO: check for maximum supported orders
   // TODO: check for supported coefficient types?

   // Batched LOR requires all tensor elements
   if (!UsesTensorBasis(*a.FESpace())) { return false; }

   if (dynamic_cast<const H1_FECollection*>(fec))
   {
      if (HasIntegrators<DiffusionIntegrator, MassIntegrator>(a)) { return true; }
   }
   else if (dynamic_cast<const ND_FECollection*>(fec))
   {
      if (HasIntegrators<CurlCurlIntegrator, VectorFEMassIntegrator>(a)) { return true; }
   }
   else if (dynamic_cast<const RT_FECollection*>(fec))
   {
      if (HasIntegrators<DivDivIntegrator, VectorFEMassIntegrator>(a)) { return true; }
   }
   return false;
}

void BatchedLORAssembly::GetLORVertexCoordinates()
{
   Mesh &mesh_ho = *fes_ho.GetMesh();
   mesh_ho.EnsureNodes();

   // Get nodal points at the LOR vertices
   const int dim = mesh_ho.Dimension();
   const int nel_ho = mesh_ho.GetNE();
   const int order = fes_ho.GetMaxElementOrder();
   const int nd1d = order + 1;
   const int ndof_per_el = pow(nd1d, dim);

   const GridFunction *nodal_gf = mesh_ho.GetNodes();
   const FiniteElementSpace *nodal_fes = nodal_gf->FESpace();
   const Operator *nodal_restriction =
      nodal_fes->GetElementRestriction(ElementDofOrdering::LEXICOGRAPHIC);

   // Map from nodal L-vector to E-vector
   Vector nodal_evec(nodal_restriction->Height());
   nodal_restriction->Mult(*nodal_gf, nodal_evec);

   IntegrationRules irs(0, Quadrature1D::GaussLobatto);
   Geometry::Type geom = mesh_ho.GetElementGeometry(0);
   const IntegrationRule &ir = irs.Get(geom, 2*nd1d - 3);

   // Map from nodal E-vector to Q-vector at the LOR vertex points
   X_vert.SetSize(dim*ndof_per_el*nel_ho);
   const QuadratureInterpolator *quad_interp =
      nodal_fes->GetQuadratureInterpolator(ir);
   quad_interp->SetOutputLayout(QVectorLayout::byVDIM);
   quad_interp->Values(nodal_evec, X_vert);
}

static MFEM_HOST_DEVICE int GetMinElt(const int *my_elts, const int nbElts,
                                      const int *nbr_elts, const int nbrNbElts)
{
   // Find the minimal element index found in both my_elts[] and nbr_elts[]
   int min_el = INT_MAX;
   for (int i = 0; i < nbElts; i++)
   {
      const int e_i = my_elts[i];
      if (e_i >= min_el) { continue; }
      for (int j = 0; j < nbrNbElts; j++)
      {
         if (e_i==nbr_elts[j])
         {
            min_el = e_i; // we already know e_i < min_el
            break;
         }
      }
   }
   return min_el;
}

int BatchedLORAssembly::FillI(SparseMatrix &A) const
{
   static constexpr int Max = 16;

   const int nvdof = fes_ho.GetVSize();

   const int ndof_per_el = fes_ho.GetFE(0)->GetDof();
   const int nel_ho = fes_ho.GetNE();
   const int nnz_per_row = sparse_mapping.Height();

   const ElementDofOrdering ordering = ElementDofOrdering::LEXICOGRAPHIC;
   const Operator *op = fes_ho.GetElementRestriction(ordering);
   const ElementRestriction *el_restr =
      dynamic_cast<const ElementRestriction*>(op);
   MFEM_VERIFY(el_restr != nullptr, "Bad element restriction");

   const Array<int> &el_dof_lex_ = el_restr->GatherMap();
   const Array<int> &dof_glob2loc_ = el_restr->Indices();
   const Array<int> &dof_glob2loc_offsets_ = el_restr->Offsets();

   const auto el_dof_lex = Reshape(el_dof_lex_.Read(), ndof_per_el, nel_ho);
   const auto dof_glob2loc = dof_glob2loc_.Read();
   const auto K = dof_glob2loc_offsets_.Read();
   const auto map = Reshape(sparse_mapping.Read(), nnz_per_row, ndof_per_el);

   auto I = A.WriteI();

   MFEM_FORALL(ii, nvdof + 1, I[ii] = 0;);
   MFEM_FORALL(i, ndof_per_el*nel_ho,
   {
      const int ii_el = i%ndof_per_el;
      const int iel_ho = i/ndof_per_el;
      const int sii = el_dof_lex(ii_el, iel_ho);
      const int ii = (sii >= 0) ? sii : -1 -sii;
      // Get number and list of elements containing this DOF
      int i_elts[Max];
      const int i_offset = K[ii];
      const int i_next_offset = K[ii+1];
      const int i_ne = i_next_offset - i_offset;
      for (int e_i = 0; e_i < i_ne; ++e_i)
      {
         const int si_E = dof_glob2loc[i_offset+e_i]; // signed
         const int i_E = (si_E >= 0) ? si_E : -1 - si_E;
         i_elts[e_i] = i_E/ndof_per_el;
      }
      for (int j = 0; j < nnz_per_row; ++j)
      {
         int jj_el = map(j, ii_el);
         if (jj_el < 0) { continue; }
         // LDOF index of column
         const int sjj = el_dof_lex(jj_el, iel_ho); // signed
         const int jj = (sjj >= 0) ? sjj : -1 - sjj;
         const int j_offset = K[jj];
         const int j_next_offset = K[jj+1];
         const int j_ne = j_next_offset - j_offset;
         if (i_ne == 1 || j_ne == 1) // no assembly required
         {
            AtomicAdd(I[ii], 1);
         }
         else // assembly required
         {
            int j_elts[Max];
            for (int e_j = 0; e_j < j_ne; ++e_j)
            {
               const int sj_E = dof_glob2loc[j_offset+e_j]; // signed
               const int j_E = (sj_E >= 0) ? sj_E : -1 - sj_E;
               const int elt = j_E/ndof_per_el;
               j_elts[e_j] = elt;
            }
            const int min_e = GetMinElt(i_elts, i_ne, j_elts, j_ne);
            if (iel_ho == min_e) // add the nnz only once
            {
               AtomicAdd(I[ii], 1);
            }
         }
      }
   });
   // TODO: on device, this is a scan operation
   // We need to sum the entries of I, we do it on CPU as it is very sequential.
   auto h_I = A.HostReadWriteI();
   int sum = 0;
   for (int i = 0; i < nvdof; i++)
   {
      const int nnz = h_I[i];
      h_I[i] = sum;
      sum+=nnz;
   }
   h_I[nvdof] = sum;

   // Return the number of nnz
   return h_I[nvdof];
}

// Returns the index where a non-zero entry should be added and increment the
// number of non-zeros for the row i_L.
static MFEM_HOST_DEVICE int GetAndIncrementNnzIndex(const int i_L, int* I)
{
   int ind = AtomicAdd(I[i_L],1);
   return ind;
}

void BatchedLORAssembly::FillJAndData(SparseMatrix &A) const
{
   const int nvdof = fes_ho.GetVSize();
   const int ndof_per_el = fes_ho.GetFE(0)->GetDof();
   const int nel_ho = fes_ho.GetNE();
   const int nnz_per_row = sparse_mapping.Height();

   const ElementDofOrdering ordering = ElementDofOrdering::LEXICOGRAPHIC;
   const Operator *op = fes_ho.GetElementRestriction(ordering);
   const ElementRestriction *el_restr =
      dynamic_cast<const ElementRestriction*>(op);
   MFEM_VERIFY(el_restr != nullptr, "Bad element restriction");

   const Array<int> &el_dof_lex_ = el_restr->GatherMap();
   const Array<int> &dof_glob2loc_ = el_restr->Indices();
   const Array<int> &dof_glob2loc_offsets_ = el_restr->Offsets();

   const auto el_dof_lex = Reshape(el_dof_lex_.Read(), ndof_per_el, nel_ho);
   const auto dof_glob2loc = dof_glob2loc_.Read();
   const auto K = dof_glob2loc_offsets_.Read();

   const auto V = Reshape(sparse_ij.Read(), nnz_per_row, ndof_per_el, nel_ho);
   const auto map = Reshape(sparse_mapping.Read(), nnz_per_row, ndof_per_el);

   Array<int> I_(nvdof + 1);
   const auto I = I_.Write();
   const auto J = A.WriteJ();
   auto AV = A.WriteData();

   // Copy A.I into I, use it as a temporary buffer
   {
      const auto I2 = A.ReadI();
      MFEM_FORALL(i, nvdof + 1, I[i] = I2[i];);
   }

   static constexpr int Max = 16;

   MFEM_FORALL(i, ndof_per_el*nel_ho,
   {
      const int ii_el = i%ndof_per_el;
      const int iel_ho = i/ndof_per_el;
      // LDOF index of current row
      const int sii = el_dof_lex(ii_el, iel_ho); // signed
      const int ii = (sii >= 0) ? sii : -1 - sii;
      // Get number and list of elements containing this DOF
      int i_elts[Max];
      int i_B[Max];
      const int i_offset = K[ii];
      const int i_next_offset = K[ii+1];
      const int i_ne = i_next_offset - i_offset;
      for (int e_i = 0; e_i < i_ne; ++e_i)
      {
         const int si_E = dof_glob2loc[i_offset+e_i]; // signed
         const bool plus = si_E >= 0;
         const int i_E = plus ? si_E : -1 - si_E;
         i_elts[e_i] = i_E/ndof_per_el;
         const double i_Bi = i_E%ndof_per_el;
         i_B[e_i] = plus ? i_Bi : -1 - i_Bi; // encode with sign
      }
      for (int j=0; j<nnz_per_row; ++j)
      {
         int jj_el = map(j, ii_el);
         if (jj_el < 0) { continue; }
         // LDOF index of column
         const int sjj = el_dof_lex(jj_el, iel_ho); // signed
         const int jj = (sjj >= 0) ? sjj : -1 - sjj;
         const int sgn = ((sjj >=0 && sii >= 0) || (sjj < 0 && sii <0)) ? 1 : -1;
         const int j_offset = K[jj];
         const int j_next_offset = K[jj+1];
         const int j_ne = j_next_offset - j_offset;
         if (i_ne == 1 || j_ne == 1) // no assembly required
         {
            const int nnz = GetAndIncrementNnzIndex(ii, I);
            J[nnz] = jj;
            AV[nnz] = sgn*V(j, ii_el, iel_ho);
         }
         else // assembly required
         {
            int j_elts[Max];
            int j_B[Max];
            for (int e_j = 0; e_j < j_ne; ++e_j)
            {
               const int sj_E = dof_glob2loc[j_offset+e_j]; // signed
               const bool plus = sj_E >= 0;
               const int j_E = plus ? sj_E : -1 - sj_E;
               j_elts[e_j] = j_E/ndof_per_el;
               const double j_Bj = j_E%ndof_per_el;
               j_B[e_j] = plus ? j_Bj : -1 - j_Bj; // encode with sign
            }
            const int min_e = GetMinElt(i_elts, i_ne, j_elts, j_ne);
            if (iel_ho == min_e) // add the nnz only once
            {
               double val = 0.0;
               for (int k = 0; k < i_ne; k++)
               {
                  const int iel_ho_2 = i_elts[k];
                  const int sii_el_2 = i_B[k]; // signed
                  const int ii_el_2 = (sii_el_2 >= 0) ? sii_el_2 : -1 -sii_el_2;
                  for (int l = 0; l < j_ne; l++)
                  {
                     const int jel_ho_2 = j_elts[l];
                     if (iel_ho_2 == jel_ho_2)
                     {
                        const int sjj_el_2 = j_B[l]; // signed
                        const int jj_el_2 = (sjj_el_2 >= 0) ? sjj_el_2 : -1 -sjj_el_2;
                        const int sgn_2 = ((sjj_el_2 >=0 && sii_el_2 >= 0)
                                           || (sjj_el_2 < 0 && sii_el_2 <0)) ? 1 : -1;
                        int j2 = -1;
                        // find nonzero in matrix of other element
                        for (int m = 0; m < nnz_per_row; ++m)
                        {
                           if (map(m, ii_el_2) == jj_el_2)
                           {
                              j2 = m;
                              break;
                           }
                        }
                        MFEM_ASSERT_KERNEL(j >= 0, "Can't find nonzero");
                        val += sgn_2*V(j2, ii_el_2, iel_ho_2);
                     }
                  }
               }
               const int nnz = GetAndIncrementNnzIndex(ii, I);
               J[nnz] = jj;
               AV[nnz] = val;
            }
         }
      }
   });
}

void BatchedLORAssembly::SparseIJToCSR(OperatorHandle &A) const
{
   const int nvdof = fes_ho.GetVSize();

   // If A contains an existing SparseMatrix, reuse it (and try to reuse its
   // I, J, A arrays if they are big enough)
   SparseMatrix *A_mat = A.Is<SparseMatrix>();
   if (!A_mat)
   {
      A_mat = new SparseMatrix;
      A.Reset(A_mat);
   }

   A_mat->OverrideSize(nvdof, nvdof);

   A_mat->GetMemoryI().New(nvdof+1, Device::GetDeviceMemoryType());
   int nnz = FillI(*A_mat);

   A_mat->GetMemoryJ().New(nnz, Device::GetDeviceMemoryType());
   A_mat->GetMemoryData().New(nnz, Device::GetDeviceMemoryType());
   FillJAndData(*A_mat);
}

void BatchedLORAssembly::AssembleWithoutBC(OperatorHandle &A)
{
   // Assemble the matrix, using kernels from the derived classes
   // This fills in the arrays sparse_ij and sparse_mapping
   AssemblyKernel();
   return SparseIJToCSR(A);
}

#ifdef MFEM_USE_MPI
void BatchedLORAssembly::ParAssemble(OperatorHandle &A)
{
   // Assemble the system matrix local to this partition
   OperatorHandle A_local;
   AssembleWithoutBC(A_local);

   ParFiniteElementSpace *pfes_ho =
      dynamic_cast<ParFiniteElementSpace*>(&fes_ho);
   MFEM_VERIFY(pfes_ho != nullptr,
               "ParAssemble must be called with ParFiniteElementSpace");

   // Create a block diagonal parallel matrix
   OperatorHandle A_diag(Operator::Hypre_ParCSR);
   A_diag.MakeSquareBlockDiag(pfes_ho->GetComm(),
                              pfes_ho->GlobalVSize(),
                              pfes_ho->GetDofOffsets(),
                              A_local.As<SparseMatrix>());

   // Parallel matrix assembly using P^t A P (if needed)
   if (IsIdentityProlongation(pfes_ho->GetProlongationMatrix()))
   {
      A_diag.SetOperatorOwner(false);
      A.Reset(A_diag.Ptr());
      HypreStealOwnership(*A.As<HypreParMatrix>(), *A_local.As<SparseMatrix>());
   }
   else
   {
      OperatorHandle P(Operator::Hypre_ParCSR);
      P.ConvertFrom(pfes_ho->Dof_TrueDof_Matrix());
      A.MakePtAP(A_diag, P);
   }

   // Eliminate the boundary conditions
   HypreParMatrix *A_mat = A.As<HypreParMatrix>();
   hypre_ParCSRMatrix *A_hypre = *A_mat;
   A_mat->HypreReadWrite();

   hypre_CSRMatrix *diag = hypre_ParCSRMatrixDiag(A_hypre);
   hypre_CSRMatrix *offd = hypre_ParCSRMatrixOffd(A_hypre);

   HYPRE_Int diag_nrows = hypre_CSRMatrixNumRows(diag);
   HYPRE_Int offd_ncols = hypre_CSRMatrixNumCols(offd);

   const int n_ess_dofs = ess_dofs.Size();
   const auto ess_dofs_d = ess_dofs.GetMemory().Read(
                              GetHypreMemoryClass(), n_ess_dofs);

   // Start communication to figure out which columns need to be eliminated in
   // the off-diagonal block
   hypre_ParCSRCommHandle *comm_handle;
   HYPRE_Int *int_buf_data, *eliminate_row, *eliminate_col;
   {
      eliminate_row = hypre_CTAlloc(HYPRE_Int, diag_nrows, HYPRE_MEMORY_HOST);
      eliminate_col = hypre_CTAlloc(HYPRE_Int, offd_ncols, HYPRE_MEMORY_HOST);

      // Make sure A has a communication package
      hypre_ParCSRCommPkg *comm_pkg = hypre_ParCSRMatrixCommPkg(A_hypre);
      if (!comm_pkg)
      {
         hypre_MatvecCommPkgCreate(A_hypre);
         comm_pkg = hypre_ParCSRMatrixCommPkg(A_hypre);
      }

      // Which of the local rows are to be eliminated?
      for (int i = 0; i < diag_nrows; i++)
      {
         eliminate_row[i] = 0;
      }

      ess_dofs.HostRead();
      for (int i = 0; i < n_ess_dofs; i++)
      {
         eliminate_row[ess_dofs[i]] = 1;
      }

      // Use a matvec communication pattern to find (in eliminate_col) which of
      // the local offd columns are to be eliminated
      HYPRE_Int num_sends = hypre_ParCSRCommPkgNumSends(comm_pkg);
      int_buf_data =
         hypre_CTAlloc(HYPRE_Int,
                       hypre_ParCSRCommPkgSendMapStart(comm_pkg, num_sends),
                       HYPRE_MEMORY_HOST);
      int index = 0;
      for (int i = 0; i < num_sends; i++)
      {
         int start = hypre_ParCSRCommPkgSendMapStart(comm_pkg, i);
         for (int j = start; j < hypre_ParCSRCommPkgSendMapStart(comm_pkg, i+1); j++)
         {
            int k = hypre_ParCSRCommPkgSendMapElmt(comm_pkg,j);
            int_buf_data[index++] = eliminate_row[k];
         }
      }
      comm_handle = hypre_ParCSRCommHandleCreate(
                       11, comm_pkg, int_buf_data, eliminate_col);
   }

   // Eliminate rows and columns in the diagonal block
   {
      const auto I = diag->i;
      const auto J = diag->j;
      auto data = diag->data;

      MFEM_HYPRE_FORALL(i, n_ess_dofs,
      {
         const int idof = ess_dofs_d[i];
         for (int j=I[idof]; j<I[idof+1]; ++j)
         {
            const int jdof = J[j];
            if (jdof == idof)
            {
               // Set eliminate diagonal equal to identity
               data[j] = 1.0;
            }
            else
            {
               data[j] = 0.0;
               for (int k=I[jdof]; k<I[jdof+1]; ++k)
               {
                  if (J[k] == idof)
                  {
                     data[k] = 0.0;
                     break;
                  }
               }
            }
         }
      });
   }

   // Eliminate rows in the off-diagonal block
   {
      const auto I = offd->i;
      auto data = offd->data;
      MFEM_HYPRE_FORALL(i, n_ess_dofs,
      {
         const int idof = ess_dofs_d[i];
         for (int j=I[idof]; j<I[idof+1]; ++j)
         {
            data[j] = 0.0;
         }
      });
   }

   // Wait for MPI communication to finish
   Array<HYPRE_Int> cols_to_eliminate;
   {
      hypre_ParCSRCommHandleDestroy(comm_handle);

      // set the array cols_to_eliminate
      int ncols_to_eliminate = 0;
      for (int i = 0; i < offd_ncols; i++)
      {
         if (eliminate_col[i]) { ncols_to_eliminate++; }
      }

      cols_to_eliminate.SetSize(ncols_to_eliminate);
      cols_to_eliminate = 0.0;

      ncols_to_eliminate = 0;
      for (int i = 0; i < offd_ncols; i++)
      {
         if (eliminate_col[i])
         {
            cols_to_eliminate[ncols_to_eliminate++] = i;
         }
      }

      hypre_TFree(int_buf_data, HYPRE_MEMORY_HOST);
      hypre_TFree(eliminate_row, HYPRE_MEMORY_HOST);
      hypre_TFree(eliminate_col, HYPRE_MEMORY_HOST);
   }

   // Eliminate columns in the off-diagonal block
   {
      const int ncols_to_eliminate = cols_to_eliminate.Size();
      const int nrows_offd = hypre_CSRMatrixNumRows(offd);
      const auto cols = cols_to_eliminate.GetMemory().Read(
                           GetHypreMemoryClass(), ncols_to_eliminate);
      const auto I = offd->i;
      const auto J = offd->j;
      auto data = offd->data;
      // Note: could also try a different strategy, looping over nnz in the
      // matrix and then doing a binary search in ncols_to_eliminate to see if
      // the column should be eliminated.
      MFEM_HYPRE_FORALL(idx, ncols_to_eliminate,
      {
         const int j = cols[idx];
         for (int i=0; i<nrows_offd; ++i)
         {
            for (int jj=I[i]; jj<I[i+1]; ++jj)
            {
               if (J[jj] == j)
               {
                  data[jj] = 0.0;
                  break;
               }
            }
         }
      });
   }
}
#endif

void BatchedLORAssembly::Assemble(OperatorHandle &A)
{
#ifdef MFEM_USE_MPI
   if (dynamic_cast<ParFiniteElementSpace*>(&fes_ho))
   {
      return ParAssemble(A);
   }
#endif

   AssembleWithoutBC(A);
   SparseMatrix *A_mat = A.As<SparseMatrix>();

   // Eliminate essential DOFs (BCs) from the matrix (what we do here is
   // equivalent to  DiagonalPolicy::DIAG_KEEP).
   const int n_ess_dofs = ess_dofs.Size();
   const auto ess_dofs_d = ess_dofs.Read();
   const auto I = A_mat->ReadI();
   const auto J = A_mat->ReadJ();
   auto dA = A_mat->ReadWriteData();

   MFEM_FORALL(i, n_ess_dofs,
   {
      const int idof = ess_dofs_d[i];
      for (int j=I[idof]; j<I[idof+1]; ++j)
      {
         const int jdof = J[j];
         if (jdof != idof)
         {
            dA[j] = 0.0;
            for (int k=I[jdof]; k<I[jdof+1]; ++k)
            {
               if (J[k] == idof)
               {
                  dA[k] = 0.0;
                  break;
               }
            }
         }
      }
   });
}

BatchedLORAssembly::BatchedLORAssembly(BilinearForm &a,
                                       FiniteElementSpace &fes_ho_,
                                       const Array<int> &ess_dofs_)
   : fes_ho(fes_ho_), ess_dofs(ess_dofs_)
{
   GetLORVertexCoordinates();
}

void BatchedLORAssembly::Assemble(BilinearForm &a,
                                  FiniteElementSpace &fes_ho,
                                  const Array<int> &ess_dofs,
                                  OperatorHandle &A)
{
   const FiniteElementCollection *fec = fes_ho.FEColl();
   if (dynamic_cast<const H1_FECollection*>(fec))
   {
      if (HasIntegrators<DiffusionIntegrator, MassIntegrator>(a))
      {
         BatchedLOR_H1(a, fes_ho, ess_dofs).Assemble(A);
      }
   }
   else if (dynamic_cast<const ND_FECollection*>(fec))
   {
      if (HasIntegrators<CurlCurlIntegrator, VectorFEMassIntegrator>(a))
      {
         BatchedLOR_ND(a, fes_ho, ess_dofs).Assemble(A);
      }
   }
   else if (dynamic_cast<const RT_FECollection*>(fec))
   {
      if (HasIntegrators<DivDivIntegrator, VectorFEMassIntegrator>(a))
      {
         BatchedLOR_RT(a, fes_ho, ess_dofs).Assemble(A);
      }
   }
}

} // namespace mfem
