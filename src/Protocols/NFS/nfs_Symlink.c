/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    nfs_Symlink.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:54 $
 * \version $Revision: 1.17 $
 * \brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * nfs_Symlink.c : Routines used for managing the NFS4 COMPOUND functions.
 *
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>           /* for having FNDELAY */
#include "HashData.h"
#include "HashTable.h"
#include "log.h"
#include "ganesha_rpc.h"
#include "nfs23.h"
#include "nfs4.h"
#include "mount.h"
#include "nfs_core.h"
#include "cache_inode.h"
#include "nfs_exports.h"
#include "nfs_creds.h"
#include "nfs_proto_functions.h"
#include "nfs_tools.h"
#include "nfs_file_handle.h"
#include "nfs_proto_tools.h"

/**
 *
 * @brief The NFS PROC2 and PROC3 SYMLINK
 *
 * Implements the NFS PROC SYMLINK function (for V2 and V3).
 *
 * @param[in]  parg     NFS argument union
 * @param[in]  pexport  NFS export list
 * @param[in]  pcontext Credentials to be used for this request
 * @param[in]  pworker  Worker thread data
 * @param[in]  preq     SVC request related to this call
 * @param[out] pres     Structure to contain the result of the call
 *
 * @retval NFS_REQ_OK if successful
 * @retval NFS_REQ_DROP if failed but retryable
 * @retval NFS_REQ_FAILED if failed and not retryable
 *
 */

int nfs_Symlink(nfs_arg_t *parg,
                exportlist_t *pexport,
                fsal_op_context_t *pcontext,
                nfs_worker_data_t *pworker,
                struct svc_req *preq,
                nfs_res_t *pres)
{
  char *str_symlink_name = NULL;
  fsal_name_t symlink_name;
  char *str_target_path = NULL;
  cache_inode_create_arg_t create_arg;
  fsal_accessmode_t mode = 0777;
  cache_entry_t *symlink_pentry = NULL;
  cache_entry_t *parent_pentry;
  cache_inode_file_type_t parent_filetype;
  fsal_attrib_list_t parent_attr;
  fsal_attrib_list_t attr_symlink;
  fsal_attrib_list_t attributes_symlink;
  fsal_attrib_list_t attr_parent_after;
  fsal_attrib_list_t *ppre_attr;
  cache_inode_status_t cache_status;
  cache_inode_status_t cache_status_parent;
  fsal_handle_t *pfsal_handle;
  int rc = NFS_REQ_OK;
#ifdef _USE_QUOTA
  fsal_status_t fsal_status ;
#endif

  memset(&create_arg, 0, sizeof(create_arg));

  if(isDebug(COMPONENT_NFSPROTO))
    {
      char str[LEN_FH_STR];

      switch (preq->rq_vers)
        {
        case NFS_V2:
          str_symlink_name = parg->arg_symlink2.from.name;
          str_target_path = parg->arg_symlink2.to;
          break;
        case NFS_V3:
          str_symlink_name = parg->arg_symlink3.where.name;
          str_target_path = parg->arg_symlink3.symlink.symlink_data;
          break;
        }

      nfs_FhandleToStr(preq->rq_vers,
                       &(parg->arg_symlink2.from.dir),
                       &(parg->arg_symlink3.where.dir),
                       NULL,
                       str);
      LogDebug(COMPONENT_NFSPROTO,
               "REQUEST PROCESSING: Calling nfs_Symlink handle: %s name: %s target: %s",
               str, str_symlink_name, str_target_path);
    }

  if(preq->rq_vers == NFS_V3)
    {
      /* to avoid setting it on each error case */
      pres->res_symlink3.SYMLINK3res_u.resfail.dir_wcc.before.attributes_follow = FALSE;
      pres->res_symlink3.SYMLINK3res_u.resfail.dir_wcc.after.attributes_follow = FALSE;
      ppre_attr = NULL;
    }

  /* Convert directory file handle into a vnode */
  if((parent_pentry = nfs_FhandleToCache(preq->rq_vers,
                                         &(parg->arg_symlink2.from.dir),
                                         &(parg->arg_symlink3.where.dir),
                                         NULL,
                                         &(pres->res_stat2),
                                         &(pres->res_symlink3.status),
                                         NULL,
                                         &parent_attr,
                                         pcontext, &rc)) == NULL)
    {
      /* Stale NFS FH ? */
      goto out;;
    }

  /* get directory attributes before action (for V3 reply) */
  ppre_attr = &parent_attr;

  /* Extract the filetype */
  parent_filetype = cache_inode_fsal_type_convert(parent_attr.type);

  /*
   * Sanity checks: new directory name must be non-null; parent must be
   * a directory. 
   */
  if(parent_filetype != DIRECTORY)
    {
      switch (preq->rq_vers)
        {
        case NFS_V2:
          pres->res_stat2 = NFSERR_NOTDIR;
          break;

        case NFS_V3:
          pres->res_symlink3.status = NFS3ERR_NOTDIR;
          break;
        }

      rc = NFS_REQ_OK;
      goto out;
    }

#ifdef _USE_QUOTA
    /* if quota support is active, then we should check is the FSAL allows inode creation or not */
    fsal_status = FSAL_check_quota( pexport->fullpath, 
                                    FSAL_QUOTA_INODES,
                                    FSAL_OP_CONTEXT_TO_UID( pcontext ) ) ;
    if( FSAL_IS_ERROR( fsal_status ) )
     {

       switch (preq->rq_vers)
         {
           case NFS_V2:
             pres->res_stat2 = NFSERR_DQUOT ;
             break;

           case NFS_V3:
             pres->res_symlink3.status = NFS3ERR_DQUOT;
             break;
         }

       rc = NFS_REQ_OK;
       goto out;
     }
#endif /* _USE_QUOTA */


  switch (preq->rq_vers)
    {
    case NFS_V2:
      str_symlink_name = parg->arg_symlink2.from.name;
      str_target_path = parg->arg_symlink2.to;
      break;

    case NFS_V3:
      str_symlink_name = parg->arg_symlink3.where.name;
      str_target_path = parg->arg_symlink3.symlink.symlink_data;
      break;
    }

  if(str_symlink_name == NULL ||
     *str_symlink_name == '\0'||
     str_target_path == NULL  ||
     *str_target_path == '\0' ||
     FSAL_IS_ERROR(FSAL_str2name(str_symlink_name, FSAL_MAX_NAME_LEN, &symlink_name)) ||
     FSAL_IS_ERROR(FSAL_str2path
                   (str_target_path, FSAL_MAX_PATH_LEN, &create_arg.link_content)))
    {
      cache_status = CACHE_INODE_INVALID_ARGUMENT;
    }
  else
    {
      /* Make the symlink */
      if((symlink_pentry = cache_inode_create(parent_pentry,
                                              &symlink_name,
                                              SYMBOLIC_LINK,
                                              mode,
                                              &create_arg,
                                              &attr_symlink,
                                              pcontext, &cache_status)) != NULL)
        {
          switch (preq->rq_vers)
            {
            case NFS_V2:
              pres->res_stat2 = NFS_OK;
              break;

            case NFS_V3:
              /* Build file handle */
              pfsal_handle = &symlink_pentry->handle;

              /* Some clients (like the Spec NFS benchmark) set attributes with the NFSPROC3_SYMLINK request */
              if(nfs3_Sattr_To_FSALattr(&attributes_symlink,
                                        &parg->arg_symlink3.symlink.symlink_attributes) == 0)
                {
                  pres->res_create3.status = NFS3ERR_INVAL;
                  rc = NFS_REQ_OK;
                  goto out;
                }

              /* Mode is managed above (in cache_inode_create), there is no need 
               * to manage it */
              if(attributes_symlink.asked_attributes & FSAL_ATTR_MODE)
                attributes_symlink.asked_attributes &= ~FSAL_ATTR_MODE;

              /* Some clients (like Solaris 10) try to set the size of the file to 0
               * at creation time. The FSAL create empty file, so we ignore this */
              if(attributes_symlink.asked_attributes & FSAL_ATTR_SIZE)
                attributes_symlink.asked_attributes &= ~FSAL_ATTR_SIZE;

              if(attributes_symlink.asked_attributes & FSAL_ATTR_SPACEUSED)
                attributes_symlink.asked_attributes &= ~FSAL_ATTR_SPACEUSED;

              /* Are there attributes to be set (additional to the mode) ? */
              if(attributes_symlink.asked_attributes != 0ULL &&
                 attributes_symlink.asked_attributes != FSAL_ATTR_MODE)
                {
                  /* A call to cache_inode_setattr is required */
                  if(cache_inode_setattr(symlink_pentry,
                                         &attributes_symlink,
                                         pcontext, &cache_status) != CACHE_INODE_SUCCESS)
                    {
                      /* If we are here, there was an error */
                      nfs_SetFailedStatus(pcontext, pexport,
                                          preq->rq_vers,
                                          cache_status,
                                          &pres->res_dirop2.status,
                                          &pres->res_symlink3.status,
                                          NULL, NULL,
                                          parent_pentry,
                                          ppre_attr,
                                          &(pres->res_symlink3.SYMLINK3res_u.resok.
                                            dir_wcc), NULL, NULL, NULL);

                      if(nfs_RetryableError(cache_status)) {
                        rc = NFS_REQ_DROP;
                        goto out;
                      }

                      rc = NFS_REQ_OK;
                      goto out;
                    }
                }

              if ((pres->res_symlink3.status =
                   (nfs3_AllocateFH(&pres->res_symlink3.SYMLINK3res_u
                                    .resok.obj.post_op_fh3_u.handle)))
                  != NFS3_OK) {
                   pres->res_symlink3.status = NFS3ERR_IO;
                   rc = NFS_REQ_OK;
                   goto out;
              }

              if(nfs3_FSALToFhandle
                 (&pres->res_symlink3.SYMLINK3res_u.resok.obj.post_op_fh3_u.handle,
                  pfsal_handle, pexport) == 0)
                {
                  gsh_free(pres->res_symlink3.SYMLINK3res_u.resok.obj.
                           post_op_fh3_u.handle.data.data_val);

                  pres->res_symlink3.status = NFS3ERR_BADHANDLE;
                  rc = NFS_REQ_OK;
                  goto out;
                }

              /* The the parent pentry attributes for building Wcc Data */
              if(cache_inode_getattr(parent_pentry,
                                     &attr_parent_after,
                                     pcontext,
                                     &cache_status_parent)
                 != CACHE_INODE_SUCCESS)
                {
                  gsh_free(pres->res_symlink3.SYMLINK3res_u.resok.obj.
                           post_op_fh3_u.handle.data.data_val);

                  pres->res_symlink3.status = NFS3ERR_BADHANDLE;
                  rc = NFS_REQ_OK;
                  goto out;
                }

              /* Set Post Op Fh3 structure */
              pres->res_symlink3.SYMLINK3res_u.resok.obj.handle_follows = TRUE;

              /* Build entry attributes */
              nfs_SetPostOpAttr(pexport,
                                &attr_symlink,
                                &(pres->res_symlink3.SYMLINK3res_u
                                  .resok.obj_attributes));

              /* Build Weak Cache Coherency data */
              nfs_SetWccData(pexport,
                             ppre_attr,
                             &attr_parent_after,
                             &(pres->res_symlink3.SYMLINK3res_u.resok.dir_wcc));

              pres->res_symlink3.status = NFS3_OK;
              break;
            }                   /* switch */

          rc = NFS_REQ_OK;
          goto out;
        }
    }

  /* If we are here, there was an error */
  if(nfs_RetryableError(cache_status))
    {
      rc = NFS_REQ_DROP;
      goto out;
    }

  nfs_SetFailedStatus(pcontext, pexport,
                      preq->rq_vers,
                      cache_status,
                      &pres->res_stat2,
                      &pres->res_symlink3.status,
                      NULL, NULL,
                      parent_pentry,
                      ppre_attr,
                      &(pres->res_symlink3.SYMLINK3res_u.resfail.dir_wcc),
                      NULL, NULL, NULL);

  rc = NFS_REQ_OK;

out:
  /* return references */
  if (parent_pentry)
      cache_inode_put(parent_pentry);

  if (symlink_pentry)
      cache_inode_put(symlink_pentry);

  return (rc);

}                               /* nfs_Symlink */

/**
 * nfs_Symlink_Free: Frees the result structure allocated for nfs_Symlink.
 * 
 * Frees the result structure allocated for nfs_Symlink.
 * 
 * @param pres        [INOUT]   Pointer to the result structure.
 *
 */
void nfs_Symlink_Free(nfs_res_t * resp)
{
  if((resp->res_symlink3.status == NFS3_OK) &&
     (resp->res_symlink3.SYMLINK3res_u.resok.obj.handle_follows == TRUE))
    gsh_free(resp->res_symlink3.SYMLINK3res_u.resok.obj
             .post_op_fh3_u.handle.data.data_val);
}                               /* nfs_Symlink_Free */
