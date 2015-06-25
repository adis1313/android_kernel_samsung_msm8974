/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_bit.h"
#include "xfs_log.h"
#include "xfs_inum.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_ag.h"
#include "xfs_alloc.h"
#include "xfs_quota.h"
#include "xfs_mount.h"
#include "xfs_bmap_btree.h"
#include "xfs_ialloc_btree.h"
#include "xfs_dinode.h"
#include "xfs_inode.h"
#include "xfs_ialloc.h"
#include "xfs_itable.h"
#include "xfs_rtalloc.h"
#include "xfs_error.h"
#include "xfs_bmap.h"
#include "xfs_attr.h"
#include "xfs_buf_item.h"
#include "xfs_trans_space.h"
#include "xfs_utils.h"
#include "xfs_qm.h"
#include "xfs_trace.h"

/*
 * The global quota manager. There is only one of these for the entire
 * system, _not_ one per file system. XQM keeps track of the overall
 * quota functionality, including maintaining the freelist and hash
 * tables of dquots.
 */
STATIC int	xfs_qm_init_quotainos(xfs_mount_t *);
STATIC int	xfs_qm_init_quotainfo(xfs_mount_t *);
STATIC int	xfs_qm_shake(struct shrinker *, struct shrink_control *);

/*
 * We use the batch lookup interface to iterate over the dquots as it
 * currently is the only interface into the radix tree code that allows
 * fuzzy lookups instead of exact matches.  Holding the lock over multiple
 * operations is fine as all callers are used either during mount/umount
 * or quotaoff.
 */
#define XFS_DQ_LOOKUP_BATCH	32

STATIC int
xfs_qm_dquot_walk(
	struct xfs_mount	*mp,
	int			type,
	int			(*execute)(struct xfs_dquot *dqp))
{
	struct xfs_quotainfo	*qi = mp->m_quotainfo;
	struct radix_tree_root	*tree = XFS_DQUOT_TREE(qi, type);
	uint32_t		next_index;
	int			last_error = 0;
	int			skipped;
	int			nr_found;

restart:
	skipped = 0;
	next_index = 0;
	nr_found = 0;

	while (1) {
		struct xfs_dquot *batch[XFS_DQ_LOOKUP_BATCH];
		int		error = 0;
		int		i;

		mutex_lock(&qi->qi_tree_lock);
		nr_found = radix_tree_gang_lookup(tree, (void **)batch,
					next_index, XFS_DQ_LOOKUP_BATCH);
		if (!nr_found) {
			mutex_unlock(&qi->qi_tree_lock);
			break;
		}

		for (i = 0; i < nr_found; i++) {
			struct xfs_dquot *dqp = batch[i];

			next_index = be32_to_cpu(dqp->q_core.d_id) + 1;

			error = execute(batch[i]);
			if (error == EAGAIN) {
				skipped++;
				continue;
			}
			if (error && last_error != EFSCORRUPTED)
				last_error = error;
		}

		mutex_unlock(&qi->qi_tree_lock);

		/* bail out if the filesystem is corrupted.  */
		if (last_error == EFSCORRUPTED) {
			skipped = 0;
			break;
		}
	}

	if (skipped) {
		delay(1);
		goto restart;
	}

	return last_error;
}


/*
 * Purge a dquot from all tracking data structures and free it.
 */
STATIC int
xfs_qm_dqpurge(
	struct xfs_dquot	*dqp)
{
	struct xfs_mount	*mp = dqp->q_mount;
	struct xfs_quotainfo	*qi = mp->m_quotainfo;
	struct xfs_dquot	*gdqp = NULL;

	xfs_dqlock(dqp);
	if ((dqp->dq_flags & XFS_DQ_FREEING) || dqp->q_nrefs != 0) {
		xfs_dqunlock(dqp);
		return EAGAIN;
	}

	/*
	 * If this quota has a group hint attached, prepare for releasing it
	 * now.
	 */
	gdqp = dqp->q_gdquot;
	if (gdqp) {
		xfs_dqlock(gdqp);
		dqp->q_gdquot = NULL;
	}

	dqp->dq_flags |= XFS_DQ_FREEING;

	/*
	 * If we're turning off quotas, we have to make sure that, for
	 * example, we don't delete quota disk blocks while dquots are
	 * in the process of getting written to those disk blocks.
	 * This dquot might well be on AIL, and we can't leave it there
	 * if we're turning off quotas. Basically, we need this flush
	 * lock, and are willing to block on it.
	 */
	if (!xfs_dqflock_nowait(dqp)) {
		/*
		 * Block on the flush lock after nudging dquot buffer,
		 * if it is incore.
		 */
		xfs_dqflock_pushbuf_wait(dqp);
	}

	/*
	 * If we are turning this type of quotas off, we don't care
	 * about the dirty metadata sitting in this dquot. OTOH, if
	 * we're unmounting, we do care, so we flush it and wait.
	 */
	if (XFS_DQ_IS_DIRTY(dqp)) {
		int	error;

		/*
		 * We don't care about getting disk errors here. We need
		 * to purge this dquot anyway, so we go ahead regardless.
		 */
		error = xfs_qm_dqflush(dqp, SYNC_WAIT);
		if (error)
			xfs_warn(mp, "%s: dquot %p flush failed",
				__func__, dqp);
		xfs_dqflock(dqp);
	}

	ASSERT(atomic_read(&dqp->q_pincount) == 0);
	ASSERT(XFS_FORCED_SHUTDOWN(mp) ||
	       !(dqp->q_logitem.qli_item.li_flags & XFS_LI_IN_AIL));

	xfs_dqfunlock(dqp);
	xfs_dqunlock(dqp);

	radix_tree_delete(XFS_DQUOT_TREE(qi, dqp->q_core.d_flags),
			  be32_to_cpu(dqp->q_core.d_id));
	qi->qi_dquots--;

	/*
	 * We move dquots to the freelist as soon as their reference count
	 * hits zero, so it really should be on the freelist here.
	 */
	mutex_lock(&qi->qi_lru_lock);
	ASSERT(!list_empty(&dqp->q_lru));
	list_del_init(&dqp->q_lru);
	qi->qi_lru_count--;
	XFS_STATS_DEC(xs_qm_dquot_unused);
	mutex_unlock(&qi->qi_lru_lock);

	xfs_qm_dqdestroy(dqp);

	if (gdqp)
		xfs_qm_dqput(gdqp);
	return 0;
}

/*
 * Purge the dquot cache.
 */
void
xfs_qm_dqpurge_all(
	struct xfs_mount	*mp,
	uint			flags)
{
	if (flags & XFS_QMOPT_UQUOTA)
		xfs_qm_dquot_walk(mp, XFS_DQ_USER, xfs_qm_dqpurge);
	if (flags & XFS_QMOPT_GQUOTA)
		xfs_qm_dquot_walk(mp, XFS_DQ_GROUP, xfs_qm_dqpurge);
	if (flags & XFS_QMOPT_PQUOTA)
		xfs_qm_dquot_walk(mp, XFS_DQ_PROJ, xfs_qm_dqpurge);
}

/*
 * Just destroy the quotainfo structure.
 */
void
xfs_qm_unmount(
	struct xfs_mount	*mp)
{
	if (mp->m_quotainfo) {
		xfs_qm_dqpurge_all(mp, XFS_QMOPT_QUOTALL);
		xfs_qm_destroy_quotainfo(mp);
	}
}


/*
 * This is called from xfs_mountfs to start quotas and initialize all
 * necessary data structures like quotainfo.  This is also responsible for
 * running a quotacheck as necessary.  We are guaranteed that the superblock
 * is consistently read in at this point.
 *
 * If we fail here, the mount will continue with quota turned off. We don't
 * need to inidicate success or failure at all.
 */
void
xfs_qm_mount_quotas(
	xfs_mount_t	*mp)
{
	int		error = 0;
	uint		sbf;

	/*
	 * If quotas on realtime volumes is not supported, we disable
	 * quotas immediately.
	 */
	if (mp->m_sb.sb_rextents) {
		xfs_notice(mp, "Cannot turn on quotas for realtime filesystem");
		mp->m_qflags = 0;
		goto write_changes;
	}

	ASSERT(XFS_IS_QUOTA_RUNNING(mp));

	/*
	 * Allocate the quotainfo structure inside the mount struct, and
	 * create quotainode(s), and change/rev superblock if necessary.
	 */
	error = xfs_qm_init_quotainfo(mp);
	if (error) {
		/*
		 * We must turn off quotas.
		 */
		ASSERT(mp->m_quotainfo == NULL);
		mp->m_qflags = 0;
		goto write_changes;
	}
	/*
	 * If any of the quotas are not consistent, do a quotacheck.
	 */
	if (XFS_QM_NEED_QUOTACHECK(mp)) {
		error = xfs_qm_quotacheck(mp);
		if (error) {
			/* Quotacheck failed and disabled quotas. */
			return;
		}
	}
	/*
	 * If one type of quotas is off, then it will lose its
	 * quotachecked status, since we won't be doing accounting for
	 * that type anymore.
	 */
	if (!XFS_IS_UQUOTA_ON(mp))
		mp->m_qflags &= ~XFS_UQUOTA_CHKD;
	if (!(XFS_IS_GQUOTA_ON(mp) || XFS_IS_PQUOTA_ON(mp)))
		mp->m_qflags &= ~XFS_OQUOTA_CHKD;

 write_changes:
	/*
	 * We actually don't have to acquire the m_sb_lock at all.
	 * This can only be called from mount, and that's single threaded. XXX
	 */
	spin_lock(&mp->m_sb_lock);
	sbf = mp->m_sb.sb_qflags;
	mp->m_sb.sb_qflags = mp->m_qflags & XFS_MOUNT_QUOTA_ALL;
	spin_unlock(&mp->m_sb_lock);

	if (sbf != (mp->m_qflags & XFS_MOUNT_QUOTA_ALL)) {
		if (xfs_qm_write_sb_changes(mp, XFS_SB_QFLAGS)) {
			/*
			 * We could only have been turning quotas off.
			 * We aren't in very good shape actually because
			 * the incore structures are convinced that quotas are
			 * off, but the on disk superblock doesn't know that !
			 */
			ASSERT(!(XFS_IS_QUOTA_RUNNING(mp)));
			xfs_alert(mp, "%s: Superblock update failed!",
				__func__);
		}
	}

	if (error) {
		xfs_warn(mp, "Failed to initialize disk quotas.");
		return;
	}
}

/*
 * Called from the vfsops layer.
 */
void
xfs_qm_unmount_quotas(
	xfs_mount_t	*mp)
{
	/*
	 * Release the dquots that root inode, et al might be holding,
	 * before we flush quotas and blow away the quotainfo structure.
	 */
	ASSERT(mp->m_rootip);
	xfs_qm_dqdetach(mp->m_rootip);
	if (mp->m_rbmip)
		xfs_qm_dqdetach(mp->m_rbmip);
	if (mp->m_rsumip)
		xfs_qm_dqdetach(mp->m_rsumip);

	/*
	 * Release the quota inodes.
	 */
	if (mp->m_quotainfo) {
		if (mp->m_quotainfo->qi_uquotaip) {
			IRELE(mp->m_quotainfo->qi_uquotaip);
			mp->m_quotainfo->qi_uquotaip = NULL;
		}
		if (mp->m_quotainfo->qi_gquotaip) {
			IRELE(mp->m_quotainfo->qi_gquotaip);
			mp->m_quotainfo->qi_gquotaip = NULL;
		}
	}
}

STATIC int
xfs_qm_dqattach_one(
	xfs_inode_t	*ip,
	xfs_dqid_t	id,
	uint		type,
	uint		doalloc,
	xfs_dquot_t	*udqhint, /* hint */
	xfs_dquot_t	**IO_idqpp)
{
	xfs_dquot_t	*dqp;
	int		error;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	error = 0;

	/*
	 * See if we already have it in the inode itself. IO_idqpp is
	 * &i_udquot or &i_gdquot. This made the code look weird, but
	 * made the logic a lot simpler.
	 */
	dqp = *IO_idqpp;
	if (dqp) {
		trace_xfs_dqattach_found(dqp);
		return 0;
	}

	/*
	 * udqhint is the i_udquot field in inode, and is non-NULL only
	 * when the type arg is group/project. Its purpose is to save a
	 * lookup by dqid (xfs_qm_dqget) by caching a group dquot inside
	 * the user dquot.
	 */
	if (udqhint) {
		ASSERT(type == XFS_DQ_GROUP || type == XFS_DQ_PROJ);
		xfs_dqlock(udqhint);

		/*
		 * No need to take dqlock to look at the id.
		 *
		 * The ID can't change until it gets reclaimed, and it won't
		 * be reclaimed as long as we have a ref from inode and we
		 * hold the ilock.
		 */
		dqp = udqhint->q_gdquot;
		if (dqp && be32_to_cpu(dqp->q_core.d_id) == id) {
			ASSERT(*IO_idqpp == NULL);

			*IO_idqpp = xfs_qm_dqhold(dqp);
			xfs_dqunlock(udqhint);
			return 0;
		}

		/*
		 * We can't hold a dquot lock when we call the dqget code.
		 * We'll deadlock in no time, because of (not conforming to)
		 * lock ordering - the inodelock comes before any dquot lock,
		 * and we may drop and reacquire the ilock in xfs_qm_dqget().
		 */
		xfs_dqunlock(udqhint);
	}

	/*
	 * Find the dquot from somewhere. This bumps the
	 * reference count of dquot and returns it locked.
	 * This can return ENOENT if dquot didn't exist on
	 * disk and we didn't ask it to allocate;
	 * ESRCH if quotas got turned off suddenly.
	 */
	error = xfs_qm_dqget(ip->i_mount, ip, id, type,
			     doalloc | XFS_QMOPT_DOWARN, &dqp);
	if (error)
		return error;

	trace_xfs_dqattach_get(dqp);

	/*
	 * dqget may have dropped and re-acquired the ilock, but it guarantees
	 * that the dquot returned is the one that should go in the inode.
	 */
	*IO_idqpp = dqp;
	xfs_dqunlock(dqp);
	return 0;
}


/*
 * Given a udquot and gdquot, attach a ptr to the group dquot in the
 * udquot as a hint for future lookups.
 */
STATIC void
xfs_qm_dqattach_grouphint(
	xfs_dquot_t	*udq,
	xfs_dquot_t	*gdq)
{
	xfs_dquot_t	*tmp;

	xfs_dqlock(udq);

	tmp = udq->q_gdquot;
	if (tmp) {
		if (tmp == gdq)
			goto done;

		udq->q_gdquot = NULL;
		xfs_qm_dqrele(tmp);
	}

	udq->q_gdquot = xfs_qm_dqhold(gdq);
done:
	xfs_dqunlock(udq);
}


/*
 * Given a locked inode, attach dquot(s) to it, taking U/G/P-QUOTAON
 * into account.
 * If XFS_QMOPT_DQALLOC, the dquot(s) will be allocated if needed.
 * Inode may get unlocked and relocked in here, and the caller must deal with
 * the consequences.
 */
int
xfs_qm_dqattach_locked(
	xfs_inode_t	*ip,
	uint		flags)
{
	xfs_mount_t	*mp = ip->i_mount;
	uint		nquotas = 0;
	int		error = 0;

	if (!XFS_IS_QUOTA_RUNNING(mp) ||
	    !XFS_IS_QUOTA_ON(mp) ||
	    !XFS_NOT_DQATTACHED(mp, ip) ||
	    ip->i_ino == mp->m_sb.sb_uquotino ||
	    ip->i_ino == mp->m_sb.sb_gquotino)
		return 0;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));

	if (XFS_IS_UQUOTA_ON(mp)) {
		error = xfs_qm_dqattach_one(ip, ip->i_d.di_uid, XFS_DQ_USER,
						flags & XFS_QMOPT_DQALLOC,
						NULL, &ip->i_udquot);
		if (error)
			goto done;
		nquotas++;
	}

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	if (XFS_IS_OQUOTA_ON(mp)) {
		error = XFS_IS_GQUOTA_ON(mp) ?
			xfs_qm_dqattach_one(ip, ip->i_d.di_gid, XFS_DQ_GROUP,
						flags & XFS_QMOPT_DQALLOC,
						ip->i_udquot, &ip->i_gdquot) :
			xfs_qm_dqattach_one(ip, xfs_get_projid(ip), XFS_DQ_PROJ,
						flags & XFS_QMOPT_DQALLOC,
						ip->i_udquot, &ip->i_gdquot);
		/*
		 * Don't worry about the udquot that we may have
		 * attached above. It'll get detached, if not already.
		 */
		if (error)
			goto done;
		nquotas++;
	}

	/*
	 * Attach this group quota to the user quota as a hint.
	 * This WON'T, in general, result in a thrash.
	 */
	if (nquotas == 2) {
		ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
		ASSERT(ip->i_udquot);
		ASSERT(ip->i_gdquot);

		/*
		 * We do not have i_udquot locked at this point, but this check
		 * is OK since we don't depend on the i_gdquot to be accurate
		 * 100% all the time. It is just a hint, and this will
		 * succeed in general.
		 */
		if (ip->i_udquot->q_gdquot != ip->i_gdquot)
			xfs_qm_dqattach_grouphint(ip->i_udquot, ip->i_gdquot);
	}

 done:
#ifdef DEBUG
	if (!error) {
		if (XFS_IS_UQUOTA_ON(mp))
			ASSERT(ip->i_udquot);
		if (XFS_IS_OQUOTA_ON(mp))
			ASSERT(ip->i_gdquot);
	}
	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
#endif
	return error;
}

int
xfs_qm_dqattach(
	struct xfs_inode	*ip,
	uint			flags)
{
	int			error;

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	error = xfs_qm_dqattach_locked(ip, flags);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);

	return error;
}

/*
 * Release dquots (and their references) if any.
 * The inode should be locked EXCL except when this's called by
 * xfs_ireclaim.
 */
void
xfs_qm_dqdetach(
	xfs_inode_t	*ip)
{
	if (!(ip->i_udquot || ip->i_gdquot))
		return;

	trace_xfs_dquot_dqdetach(ip);

	ASSERT(ip->i_ino != ip->i_mount->m_sb.sb_uquotino);
	ASSERT(ip->i_ino != ip->i_mount->m_sb.sb_gquotino);
	if (ip->i_udquot) {
		xfs_qm_dqrele(ip->i_udquot);
		ip->i_udquot = NULL;
	}
	if (ip->i_gdquot) {
		xfs_qm_dqrele(ip->i_gdquot);
		ip->i_gdquot = NULL;
	}
}

/*
 * This initializes all the quota information that's kept in the
 * mount structure
 */
STATIC int
xfs_qm_init_quotainfo(
	xfs_mount_t	*mp)
{
	xfs_quotainfo_t *qinf;
	int		error;
	xfs_dquot_t	*dqp;

	ASSERT(XFS_IS_QUOTA_RUNNING(mp));

	qinf = mp->m_quotainfo = kmem_zalloc(sizeof(xfs_quotainfo_t), KM_SLEEP);

	/*
	 * See if quotainodes are setup, and if not, allocate them,
	 * and change the superblock accordingly.
	 */
	if ((error = xfs_qm_init_quotainos(mp))) {
		kmem_free(qinf);
		mp->m_quotainfo = NULL;
		return error;
	}

	INIT_RADIX_TREE(&qinf->qi_uquota_tree, GFP_NOFS);
	INIT_RADIX_TREE(&qinf->qi_gquota_tree, GFP_NOFS);
	mutex_init(&qinf->qi_tree_lock);

	INIT_LIST_HEAD(&qinf->qi_lru_list);
	qinf->qi_lru_count = 0;
	mutex_init(&qinf->qi_lru_lock);

	/* mutex used to serialize quotaoffs */
	mutex_init(&qinf->qi_quotaofflock);

	/* Precalc some constants */
	qinf->qi_dqchunklen = XFS_FSB_TO_BB(mp, XFS_DQUOT_CLUSTER_SIZE_FSB);
	ASSERT(qinf->qi_dqchunklen);
	qinf->qi_dqperchunk = BBTOB(qinf->qi_dqchunklen);
	do_div(qinf->qi_dqperchunk, sizeof(xfs_dqblk_t));

	mp->m_qflags |= (mp->m_sb.sb_qflags & XFS_ALL_QUOTA_CHKD);

	/*
	 * We try to get the limits from the superuser's limits fields.
	 * This is quite hacky, but it is standard quota practice.
	 *
	 * We look at the USR dquot with id == 0 first, but if user quotas
	 * are not enabled we goto the GRP dquot with id == 0.
	 * We don't really care to keep separate default limits for user
	 * and group quotas, at least not at this point.
	 *
	 * Since we may not have done a quotacheck by this point, just read
	 * the dquot without attaching it to any hashtables or lists.
	 */
	error = xfs_qm_dqread(mp, 0,
			XFS_IS_UQUOTA_RUNNING(mp) ? XFS_DQ_USER :
			 (XFS_IS_GQUOTA_RUNNING(mp) ? XFS_DQ_GROUP :
			  XFS_DQ_PROJ),
			XFS_QMOPT_DOWARN, &dqp);
	if (!error) {
		xfs_disk_dquot_t	*ddqp = &dqp->q_core;

		/*
		 * The warnings and timers set the grace period given to
		 * a user or group before he or she can not perform any
		 * more writing. If it is zero, a default is used.
		 */
		qinf->qi_btimelimit = ddqp->d_btimer ?
			be32_to_cpu(ddqp->d_btimer) : XFS_QM_BTIMELIMIT;
		qinf->qi_itimelimit = ddqp->d_itimer ?
			be32_to_cpu(ddqp->d_itimer) : XFS_QM_ITIMELIMIT;
		qinf->qi_rtbtimelimit = ddqp->d_rtbtimer ?
			be32_to_cpu(ddqp->d_rtbtimer) : XFS_QM_RTBTIMELIMIT;
		qinf->qi_bwarnlimit = ddqp->d_bwarns ?
			be16_to_cpu(ddqp->d_bwarns) : XFS_QM_BWARNLIMIT;
		qinf->qi_iwarnlimit = ddqp->d_iwarns ?
			be16_to_cpu(ddqp->d_iwarns) : XFS_QM_IWARNLIMIT;
		qinf->qi_rtbwarnlimit = ddqp->d_rtbwarns ?
			be16_to_cpu(ddqp->d_rtbwarns) : XFS_QM_RTBWARNLIMIT;
		qinf->qi_bhardlimit = be64_to_cpu(ddqp->d_blk_hardlimit);
		qinf->qi_bsoftlimit = be64_to_cpu(ddqp->d_blk_softlimit);
		qinf->qi_ihardlimit = be64_to_cpu(ddqp->d_ino_hardlimit);
		qinf->qi_isoftlimit = be64_to_cpu(ddqp->d_ino_softlimit);
		qinf->qi_rtbhardlimit = be64_to_cpu(ddqp->d_rtb_hardlimit);
		qinf->qi_rtbsoftlimit = be64_to_cpu(ddqp->d_rtb_softlimit);

		xfs_qm_dqdestroy(dqp);
	} else {
		qinf->qi_btimelimit = XFS_QM_BTIMELIMIT;
		qinf->qi_itimelimit = XFS_QM_ITIMELIMIT;
		qinf->qi_rtbtimelimit = XFS_QM_RTBTIMELIMIT;
		qinf->qi_bwarnlimit = XFS_QM_BWARNLIMIT;
		qinf->qi_iwarnlimit = XFS_QM_IWARNLIMIT;
		qinf->qi_rtbwarnlimit = XFS_QM_RTBWARNLIMIT;
	}

	qinf->qi_shrinker.shrink = xfs_qm_shake;
	qinf->qi_shrinker.seeks = DEFAULT_SEEKS;
	register_shrinker(&qinf->qi_shrinker);
	return 0;
}


/*
 * Gets called when unmounting a filesystem or when all quotas get
 * turned off.
 * This purges the quota inodes, destroys locks and frees itself.
 */
void
xfs_qm_destroy_quotainfo(
	xfs_mount_t	*mp)
{
	xfs_quotainfo_t *qi;

	qi = mp->m_quotainfo;
	ASSERT(qi != NULL);

	unregister_shrinker(&qi->qi_shrinker);

	if (qi->qi_uquotaip) {
		IRELE(qi->qi_uquotaip);
		qi->qi_uquotaip = NULL; /* paranoia */
	}
	if (qi->qi_gquotaip) {
		IRELE(qi->qi_gquotaip);
		qi->qi_gquotaip = NULL;
	}
	mutex_destroy(&qi->qi_quotaofflock);
	kmem_free(qi);
	mp->m_quotainfo = NULL;
}

/*
 * Create an inode and return with a reference already taken, but unlocked
 * This is how we create quota inodes
 */
STATIC int
xfs_qm_qino_alloc(
	xfs_mount_t	*mp,
	xfs_inode_t	**ip,
	__int64_t	sbfields,
	uint		flags)
{
	xfs_trans_t	*tp;
	int		error;
	int		committed;

	tp = xfs_trans_alloc(mp, XFS_TRANS_QM_QINOCREATE);
	if ((error = xfs_trans_reserve(tp,
				      XFS_QM_QINOCREATE_SPACE_RES(mp),
				      XFS_CREATE_LOG_RES(mp), 0,
				      XFS_TRANS_PERM_LOG_RES,
				      XFS_CREATE_LOG_COUNT))) {
		xfs_trans_cancel(tp, 0);
		return error;
	}

	error = xfs_dir_ialloc(&tp, NULL, S_IFREG, 1, 0, 0, 1, ip, &committed);
	if (error) {
		xfs_trans_cancel(tp, XFS_TRANS_RELEASE_LOG_RES |
				 XFS_TRANS_ABORT);
		return error;
	}

	/*
	 * Make the changes in the superblock, and log those too.
	 * sbfields arg may contain fields other than *QUOTINO;
	 * VERSIONNUM for example.
	 */
	spin_lock(&mp->m_sb_lock);
	if (flags & XFS_QMOPT_SBVERSION) {
		ASSERT(!xfs_sb_version_hasquota(&mp->m_sb));
		ASSERT((sbfields & (XFS_SB_VERSIONNUM | XFS_SB_UQUOTINO |
				   XFS_SB_GQUOTINO | XFS_SB_QFLAGS)) ==
		       (XFS_SB_VERSIONNUM | XFS_SB_UQUOTINO |
			XFS_SB_GQUOTINO | XFS_SB_QFLAGS));

		xfs_sb_version_addquota(&mp->m_sb);
		mp->m_sb.sb_uquotino = NULLFSINO;
		mp->m_sb.sb_gquotino = NULLFSINO;

		/* qflags will get updated _after_ quotacheck */
		mp->m_sb.sb_qflags = 0;
	}
	if (flags & XFS_QMOPT_UQUOTA)
		mp->m_sb.sb_uquotino = (*ip)->i_ino;
	else
		mp->m_sb.sb_gquotino = (*ip)->i_ino;
	spin_unlock(&mp->m_sb_lock);
	xfs_mod_sb(tp, sbfields);

	if ((error = xfs_trans_commit(tp, XFS_TRANS_RELEASE_LOG_RES))) {
		xfs_alert(mp, "%s failed (error %d)!", __func__, error);
		return error;
	}
	return 0;
}


STATIC void
xfs_qm_reset_dqcounts(
	xfs_mount_t	*mp,
	xfs_buf_t	*bp,
	xfs_dqid_t	id,
	uint		type)
{
	xfs_disk_dquot_t	*ddq;
	int			j;

	trace_xfs_reset_dqcounts(bp, _RET_IP_);

	/*
	 * Reset all counters and timers. They'll be
	 * started afresh by xfs_qm_quotacheck.
	 */
#ifdef DEBUG
	j = XFS_FSB_TO_B(mp, XFS_DQUOT_CLUSTER_SIZE_FSB);
	do_div(j, sizeof(xfs_dqblk_t));
	ASSERT(mp->m_quotainfo->qi_dqperchunk == j);
#endif
	ddq = bp->b_addr;
	for (j = 0; j < mp->m_quotainfo->qi_dqperchunk; j++) {
		/*
		 * Do a sanity check, and if needed, repair the dqblk. Don't
		 * output any warnings because it's perfectly possible to
		 * find uninitialised dquot blks. See comment in xfs_qm_dqcheck.
		 */
		(void) xfs_qm_dqcheck(mp, ddq, id+j, type, XFS_QMOPT_DQREPAIR,
				      "xfs_quotacheck");
		ddq->d_bcount = 0;
		ddq->d_icount = 0;
		ddq->d_rtbcount = 0;
		ddq->d_btimer = 0;
		ddq->d_itimer = 0;
		ddq->d_rtbtimer = 0;
		ddq->d_bwarns = 0;
		ddq->d_iwarns = 0;
		ddq->d_rtbwarns = 0;
		ddq = (xfs_disk_dquot_t *) ((xfs_dqblk_t *)ddq + 1);
	}
}

STATIC int
xfs_qm_dqiter_bufs(
	xfs_mount_t	*mp,
	xfs_dqid_t	firstid,
	xfs_fsblock_t	bno,
	xfs_filblks_t	blkcnt,
	uint		flags)
{
	xfs_buf_t	*bp;
	int		error;
	int		type;

	ASSERT(blkcnt > 0);
	type = flags & XFS_QMOPT_UQUOTA ? XFS_DQ_USER :
		(flags & XFS_QMOPT_PQUOTA ? XFS_DQ_PROJ : XFS_DQ_GROUP);
	error = 0;

	/*
	 * Blkcnt arg can be a very big number, and might even be
	 * larger than the log itself. So, we have to break it up into
	 * manageable-sized transactions.
	 * Note that we don't start a permanent transaction here; we might
	 * not be able to get a log reservation for the whole thing up front,
	 * and we don't really care to either, because we just discard
	 * everything if we were to crash in the middle of this loop.
	 */
	while (blkcnt--) {
		error = xfs_trans_read_buf(mp, NULL, mp->m_ddev_targp,
			      XFS_FSB_TO_DADDR(mp, bno),
			      mp->m_quotainfo->qi_dqchunklen, 0, &bp);
		if (error)
			break;

		xfs_qm_reset_dqcounts(mp, bp, firstid, type);
		xfs_buf_delwri_queue(bp);
		xfs_buf_relse(bp);
		/*
		 * goto the next block.
		 */
		bno++;
		firstid += mp->m_quotainfo->qi_dqperchunk;
	}
	return error;
}

/*
 * Iterate over all allocated USR/GRP/PRJ dquots in the system, calling a
 * caller supplied function for every chunk of dquots that we find.
 */
STATIC int
xfs_qm_dqiterate(
	xfs_mount_t	*mp,
	xfs_inode_t	*qip,
	uint		flags)
{
	xfs_bmbt_irec_t		*map;
	int			i, nmaps;	/* number of map entries */
	int			error;		/* return value */
	xfs_fileoff_t		lblkno;
	xfs_filblks_t		maxlblkcnt;
	xfs_dqid_t		firstid;
	xfs_fsblock_t		rablkno;
	xfs_filblks_t		rablkcnt;

	error = 0;
	/*
	 * This looks racy, but we can't keep an inode lock across a
	 * trans_reserve. But, this gets called during quotacheck, and that
	 * happens only at mount time which is single threaded.
	 */
	if (qip->i_d.di_nblocks == 0)
		return 0;

	map = kmem_alloc(XFS_DQITER_MAP_SIZE * sizeof(*map), KM_SLEEP);

	lblkno = 0;
	maxlblkcnt = XFS_B_TO_FSB(mp, (xfs_ufsize_t)XFS_MAXIOFFSET(mp));
	do {
		nmaps = XFS_DQITER_MAP_SIZE;
		/*
		 * We aren't changing the inode itself. Just changing
		 * some of its data. No new blocks are added here, and
		 * the inode is never added to the transaction.
		 */
		xfs_ilock(qip, XFS_ILOCK_SHARED);
		error = xfs_bmapi_read(qip, lblkno, maxlblkcnt - lblkno,
				       map, &nmaps, 0);
		xfs_iunlock(qip, XFS_ILOCK_SHARED);
		if (error)
			break;

		ASSERT(nmaps <= XFS_DQITER_MAP_SIZE);
		for (i = 0; i < nmaps; i++) {
			ASSERT(map[i].br_startblock != DELAYSTARTBLOCK);
			ASSERT(map[i].br_blockcount);


			lblkno += map[i].br_blockcount;

			if (map[i].br_startblock == HOLESTARTBLOCK)
				continue;

			firstid = (xfs_dqid_t) map[i].br_startoff *
				mp->m_quotainfo->qi_dqperchunk;
			/*
			 * Do a read-ahead on the next extent.
			 */
			if ((i+1 < nmaps) &&
			    (map[i+1].br_startblock != HOLESTARTBLOCK)) {
				rablkcnt =  map[i+1].br_blockcount;
				rablkno = map[i+1].br_startblock;
				while (rablkcnt--) {
					xfs_buf_readahead(mp->m_ddev_targp,
					       XFS_FSB_TO_DADDR(mp, rablkno),
					       mp->m_quotainfo->qi_dqchunklen);
					rablkno++;
				}
			}
			/*
			 * Iterate thru all the blks in the extent and
			 * reset the counters of all the dquots inside them.
			 */
			if ((error = xfs_qm_dqiter_bufs(mp,
						       firstid,
						       map[i].br_startblock,
						       map[i].br_blockcount,
						       flags))) {
				break;
			}
		}

		if (error)
			break;
	} while (nmaps > 0);

	kmem_free(map);

	return error;
}

/*
 * Called by dqusage_adjust in doing a quotacheck.
 *
 * Given the inode, and a dquot id this updates both the incore dqout as well
 * as the buffer copy. This is so that once the quotacheck is done, we can
 * just log all the buffers, as opposed to logging numerous updates to
 * individual dquots.
 */
STATIC int
xfs_qm_quotacheck_dqadjust(
	struct xfs_inode	*ip,
	xfs_dqid_t		id,
	uint			type,
	xfs_qcnt_t		nblks,
	xfs_qcnt_t		rtblks)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_dquot	*dqp;
	int			error;

	error = xfs_qm_dqget(mp, ip, id, type,
			     XFS_QMOPT_DQALLOC | XFS_QMOPT_DOWARN, &dqp);
	if (error) {
		/*
		 * Shouldn't be able to turn off quotas here.
		 */
		ASSERT(error != ESRCH);
		ASSERT(error != ENOENT);
		return error;
	}

	trace_xfs_dqadjust(dqp);

	/*
	 * Adjust the inode count and the block count to reflect this inode's
	 * resource usage.
	 */
	be64_add_cpu(&dqp->q_core.d_icount, 1);
	dqp->q_res_icount++;
	if (nblks) {
		be64_add_cpu(&dqp->q_core.d_bcount, nblks);
		dqp->q_res_bcount += nblks;
	}
	if (rtblks) {
		be64_add_cpu(&dqp->q_core.d_rtbcount, rtblks);
		dqp->q_res_rtbcount += rtblks;
	}

	/*
	 * Set default limits, adjust timers (since we changed usages)
	 *
	 * There are no timers for the default values set in the root dquot.
	 */
	if (dqp->q_core.d_id) {
		xfs_qm_adjust_dqlimits(mp, &dqp->q_core);
		xfs_qm_adjust_dqtimers(mp, &dqp->q_core);
	}

	dqp->dq_flags |= XFS_DQ_DIRTY;
	xfs_qm_dqput(dqp);
	return 0;
}

STATIC int
xfs_qm_get_rtblks(
	xfs_inode_t	*ip,
	xfs_qcnt_t	*O_rtblks)
{
	xfs_filblks_t	rtblks;			/* total rt blks */
	xfs_extnum_t	idx;			/* extent record index */
	xfs_ifork_t	*ifp;			/* inode fork pointer */
	xfs_extnum_t	nextents;		/* number of extent entries */
	int		error;

	ASSERT(XFS_IS_REALTIME_INODE(ip));
	ifp = XFS_IFORK_PTR(ip, XFS_DATA_FORK);
	if (!(ifp->if_flags & XFS_IFEXTENTS)) {
		if ((error = xfs_iread_extents(NULL, ip, XFS_DATA_FORK)))
			return error;
	}
	rtblks = 0;
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	for (idx = 0; idx < nextents; idx++)
		rtblks += xfs_bmbt_get_blockcount(xfs_iext_get_ext(ifp, idx));
	*O_rtblks = (xfs_qcnt_t)rtblks;
	return 0;
}

/*
 * callback routine supplied to bulkstat(). Given an inumber, find its
 * dquots and update them to account for resources taken by that inode.
 */
/* ARGSUSED */
STATIC int
xfs_qm_dqusage_adjust(
	xfs_mount_t	*mp,		/* mount point for filesystem */
	xfs_ino_t	ino,		/* inode number to get data for */
	void		__user *buffer,	/* not used */
	int		ubsize,		/* not used */
	int		*ubused,	/* not used */
	int		*res)		/* result code value */
{
	xfs_inode_t	*ip;
	xfs_qcnt_t	nblks, rtblks = 0;
	int		error;

	ASSERT(XFS_IS_QUOTA_RUNNING(mp));

	/*
	 * rootino must have its resources accounted for, not so with the quota
	 * inodes.
	 */
	if (ino == mp->m_sb.sb_uquotino || ino == mp->m_sb.sb_gquotino) {
		*res = BULKSTAT_RV_NOTHING;
		return XFS_ERROR(EINVAL);
	}

	/*
	 * We don't _need_ to take the ilock EXCL. However, the xfs_qm_dqget
	 * interface expects the inode to be exclusively locked because that's
	 * the case in all other instances. It's OK that we do this because
	 * quotacheck is done only at mount time.
	 */
	error = xfs_iget(mp, NULL, ino, 0, XFS_ILOCK_EXCL, &ip);
	if (error) {
		*res = BULKSTAT_RV_NOTHING;
		return error;
	}

	ASSERT(ip->i_delayed_blks == 0);

	if (XFS_IS_REALTIME_INODE(ip)) {
		/*
		 * Walk thru the extent list and count the realtime blocks.
		 */
		error = xfs_qm_get_rtblks(ip, &rtblks);
		if (error)
			goto error0;
	}

	nblks = (xfs_qcnt_t)ip->i_d.di_nblocks - rtblks;

	/*
	 * Add the (disk blocks and inode) resources occupied by this
	 * inode to its dquots. We do this adjustment in the incore dquot,
	 * and also copy the changes to its buffer.
	 * We don't care about putting these changes in a transaction
	 * envelope because if we crash in the middle of a 'quotacheck'
	 * we have to start from the beginning anyway.
	 * Once we're done, we'll log all the dquot bufs.
	 *
	 * The *QUOTA_ON checks below may look pretty racy, but quotachecks
	 * and quotaoffs don't race. (Quotachecks happen at mount time only).
	 */
	if (XFS_IS_UQUOTA_ON(mp)) {
		error = xfs_qm_quotacheck_dqadjust(ip, ip->i_d.di_uid,
						   XFS_DQ_USER, nblks, rtblks);
		if (error)
			goto error0;
	}

	if (XFS_IS_GQUOTA_ON(mp)) {
		error = xfs_qm_quotacheck_dqadjust(ip, ip->i_d.di_gid,
						   XFS_DQ_GROUP, nblks, rtblks);
		if (error)
			goto error0;
	}

	if (XFS_IS_PQUOTA_ON(mp)) {
		error = xfs_qm_quotacheck_dqadjust(ip, xfs_get_projid(ip),
						   XFS_DQ_PROJ, nblks, rtblks);
		if (error)
			goto error0;
	}

	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	IRELE(ip);
	*res = BULKSTAT_RV_DIDONE;
	return 0;

error0:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	IRELE(ip);
	*res = BULKSTAT_RV_GIVEUP;
	return error;
}

STATIC int
xfs_qm_flush_one(
	struct xfs_dquot	*dqp)
{
	int			error = 0;

	xfs_dqlock(dqp);
	if (dqp->dq_flags & XFS_DQ_FREEING)
		goto out_unlock;
	if (!XFS_DQ_IS_DIRTY(dqp))
		goto out_unlock;

	if (!xfs_dqflock_nowait(dqp))
		xfs_dqflock_pushbuf_wait(dqp);

	error = xfs_qm_dqflush(dqp, 0);

out_unlock:
	xfs_dqunlock(dqp);
	return error;
}

/*
 * Walk thru all the filesystem inodes and construct a consistent view
 * of the disk quota world. If the quotacheck fails, disable quotas.
 */
int
xfs_qm_quotacheck(
	xfs_mount_t	*mp)
{
	int		done, count, error, error2;
	xfs_ino_t	lastino;
	size_t		structsz;
	xfs_inode_t	*uip, *gip;
	uint		flags;

	count = INT_MAX;
	structsz = 1;
	lastino = 0;
	flags = 0;

	ASSERT(mp->m_quotainfo->qi_uquotaip || mp->m_quotainfo->qi_gquotaip);
	ASSERT(XFS_IS_QUOTA_RUNNING(mp));

	xfs_notice(mp, "Quotacheck needed: Please wait.");

	/*
	 * First we go thru all the dquots on disk, USR and GRP/PRJ, and reset
	 * their counters to zero. We need a clean slate.
	 * We don't log our changes till later.
	 */
	uip = mp->m_quotainfo->qi_uquotaip;
	if (uip) {
		error = xfs_qm_dqiterate(mp, uip, XFS_QMOPT_UQUOTA);
		if (error)
			goto error_return;
		flags |= XFS_UQUOTA_CHKD;
	}

	gip = mp->m_quotainfo->qi_gquotaip;
	if (gip) {
		error = xfs_qm_dqiterate(mp, gip, XFS_IS_GQUOTA_ON(mp) ?
					XFS_QMOPT_GQUOTA : XFS_QMOPT_PQUOTA);
		if (error)
			goto error_return;
		flags |= XFS_OQUOTA_CHKD;
	}

	do {
		/*
		 * Iterate thru all the inodes in the file system,
		 * adjusting the corresponding dquot counters in core.
		 */
		error = xfs_bulkstat(mp, &lastino, &count,
				     xfs_qm_dqusage_adjust,
				     structsz, NULL, &done);
		if (error)
			break;

	} while (!done);

	/*
	 * We've made all the changes that we need to make incore.  Flush them
	 * down to disk buffers if everything was updated successfully.
	 */
	if (XFS_IS_UQUOTA_ON(mp))
		error = xfs_qm_dquot_walk(mp, XFS_DQ_USER, xfs_qm_flush_one);
	if (XFS_IS_GQUOTA_ON(mp)) {
		error2 = xfs_qm_dquot_walk(mp, XFS_DQ_GROUP, xfs_qm_flush_one);
		if (!error)
			error = error2;
	}
	if (XFS_IS_PQUOTA_ON(mp)) {
		error2 = xfs_qm_dquot_walk(mp, XFS_DQ_PROJ, xfs_qm_flush_one);
		if (!error)
			error = error2;
	}

	/*
	 * We can get this error if we couldn't do a dquot allocation inside
	 * xfs_qm_dqusage_adjust (via bulkstat). We don't care about the
	 * dirty dquots that might be cached, we just want to get rid of them
	 * and turn quotaoff. The dquots won't be attached to any of the inodes
	 * at this point (because we intentionally didn't in dqget_noattach).
	 */
	if (error) {
		xfs_qm_dqpurge_all(mp, XFS_QMOPT_QUOTALL);
		goto error_return;
	}

	/*
	 * We didn't log anything, because if we crashed, we'll have to
	 * start the quotacheck from scratch anyway. However, we must make
	 * sure that our dquot changes are secure before we put the
	 * quotacheck'd stamp on the superblock. So, here we do a synchronous
	 * flush.
	 */
	xfs_flush_buftarg(mp->m_ddev_targp, 1);

	/*
	 * If one type of quotas is off, then it will lose its
	 * quotachecked status, since we won't be doing accounting for
	 * that type anymore.
	 */
	mp->m_qflags &= ~XFS_ALL_QUOTA_CHKD;
	mp->m_qflags |= flags;

 error_return:
	if (error) {
		xfs_warn(mp,
	"Quotacheck: Unsuccessful (Error %d): Disabling quotas.",
			error);
		/*
		 * We must turn off quotas.
		 */
		ASSERT(mp->m_quotainfo != NULL);
		xfs_qm_destroy_quotainfo(mp);
		if (xfs_mount_reset_sbqflags(mp)) {
			xfs_warn(mp,
				"Quotacheck: Failed to reset quota flags.");
		}
	} else
		xfs_notice(mp, "Quotacheck: Done.");
	return (error);
}

/*
 * This is called after the superblock has been read in and we're ready to
 * iget the quota inodes.
 */
STATIC int
xfs_qm_init_quotainos(
	xfs_mount_t	*mp)
{
	xfs_inode_t	*uip, *gip;
	int		error;
	__int64_t	sbflags;
	uint		flags;

	ASSERT(mp->m_quotainfo);
	uip = gip = NULL;
	sbflags = 0;
	flags = 0;

	/*
	 * Get the uquota and gquota inodes
	 */
	if (xfs_sb_version_hasquota(&mp->m_sb)) {
		if (XFS_IS_UQUOTA_ON(mp) &&
		    mp->m_sb.sb_uquotino != NULLFSINO) {
			ASSERT(mp->m_sb.sb_uquotino > 0);
			if ((error = xfs_iget(mp, NULL, mp->m_sb.sb_uquotino,
					     0, 0, &uip)))
				return XFS_ERROR(error);
		}
		if (XFS_IS_OQUOTA_ON(mp) &&
		    mp->m_sb.sb_gquotino != NULLFSINO) {
			ASSERT(mp->m_sb.sb_gquotino > 0);
			if ((error = xfs_iget(mp, NULL, mp->m_sb.sb_gquotino,
					     0, 0, &gip))) {
				if (uip)
					IRELE(uip);
				return XFS_ERROR(error);
			}
		}
	} else {
		flags |= XFS_QMOPT_SBVERSION;
		sbflags |= (XFS_SB_VERSIONNUM | XFS_SB_UQUOTINO |
			    XFS_SB_GQUOTINO | XFS_SB_QFLAGS);
	}

	/*
	 * Create the two inodes, if they don't exist already. The changes
	 * made above will get added to a transaction and logged in one of
	 * the qino_alloc calls below.  If the device is readonly,
	 * temporarily switch to read-write to do this.
	 */
	if (XFS_IS_UQUOTA_ON(mp) && uip == NULL) {
		if ((error = xfs_qm_qino_alloc(mp, &uip,
					      sbflags | XFS_SB_UQUOTINO,
					      flags | XFS_QMOPT_UQUOTA)))
			return XFS_ERROR(error);

		flags &= ~XFS_QMOPT_SBVERSION;
	}
	if (XFS_IS_OQUOTA_ON(mp) && gip == NULL) {
		flags |= (XFS_IS_GQUOTA_ON(mp) ?
				XFS_QMOPT_GQUOTA : XFS_QMOPT_PQUOTA);
		error = xfs_qm_qino_alloc(mp, &gip,
					  sbflags | XFS_SB_GQUOTINO, flags);
		if (error) {
			if (uip)
				IRELE(uip);

			return XFS_ERROR(error);
		}
	}

	mp->m_quotainfo->qi_uquotaip = uip;
	mp->m_quotainfo->qi_gquotaip = gip;

	return 0;
}

STATIC void
xfs_qm_dqfree_one(
	struct xfs_dquot	*dqp)
{
	struct xfs_mount	*mp = dqp->q_mount;
	struct xfs_quotainfo	*qi = mp->m_quotainfo;

	mutex_lock(&qi->qi_tree_lock);
	radix_tree_delete(XFS_DQUOT_TREE(qi, dqp->q_core.d_flags),
			  be32_to_cpu(dqp->q_core.d_id));

	qi->qi_dquots--;
	mutex_unlock(&qi->qi_tree_lock);

	xfs_qm_dqdestroy(dqp);
}

STATIC void
xfs_qm_dqreclaim_one(
	struct xfs_dquot	*dqp,
	struct list_head	*dispose_list)
{
	struct xfs_mount	*mp = dqp->q_mount;
	struct xfs_quotainfo	*qi = mp->m_quotainfo;
	int			error;

	if (!xfs_dqlock_nowait(dqp))
		goto out_busy;

	/*
	 * This dquot has acquired a reference in the meantime remove it from
	 * the freelist and try again.
	 */
	if (dqp->q_nrefs) {
		xfs_dqunlock(dqp);

		trace_xfs_dqreclaim_want(dqp);
		XFS_STATS_INC(xs_qm_dqwants);

		list_del_init(&dqp->q_lru);
		qi->qi_lru_count--;
		XFS_STATS_DEC(xs_qm_dquot_unused);
		return;
	}

	/*
	 * Try to grab the flush lock. If this dquot is in the process of
	 * getting flushed to disk, we don't want to reclaim it.
	 */
	if (!xfs_dqflock_nowait(dqp))
		goto out_busy;

	/*
	 * We have the flush lock so we know that this is not in the
	 * process of being flushed. So, if this is dirty, flush it
	 * DELWRI so that we don't get a freelist infested with
	 * dirty dquots.
	 */
	if (XFS_DQ_IS_DIRTY(dqp)) {
		trace_xfs_dqreclaim_dirty(dqp);

		/*
		 * We flush it delayed write, so don't bother releasing the
		 * freelist lock.
		 */
		error = xfs_qm_dqflush(dqp, 0);
		if (error) {
			xfs_warn(mp, "%s: dquot %p flush failed",
				 __func__, dqp);
		}

		/*
		 * Give the dquot another try on the freelist, as the
		 * flushing will take some time.
		 */
		goto out_busy;
	}
	xfs_dqfunlock(dqp);

	/*
	 * Prevent lookups now that we are past the point of no return.
	 */
	dqp->dq_flags |= XFS_DQ_FREEING;
	xfs_dqunlock(dqp);

	ASSERT(dqp->q_nrefs == 0);
	list_move_tail(&dqp->q_lru, dispose_list);
	qi->qi_lru_count--;
	XFS_STATS_DEC(xs_qm_dquot_unused);

	trace_xfs_dqreclaim_done(dqp);
	XFS_STATS_INC(xs_qm_dqreclaims);
	return;

out_busy:
	xfs_dqunlock(dqp);

	/*
	 * Move the dquot to the tail of the list so that we don't spin on it.
	 */
	list_move_tail(&dqp->q_lru, &qi->qi_lru_list);

	trace_xfs_dqreclaim_busy(dqp);
	XFS_STATS_INC(xs_qm_dqreclaim_misses);
}

STATIC int
xfs_qm_shake(
	struct shrinker		*shrink,
	struct shrink_control	*sc)
{
	struct xfs_quotainfo	*qi =
		container_of(shrink, struct xfs_quotainfo, qi_shrinker);
	int			nr_to_scan = sc->nr_to_scan;
	LIST_HEAD		(dispose_list);
	struct xfs_dquot	*dqp;

	if ((sc->gfp_mask & (__GFP_FS|__GFP_WAIT)) != (__GFP_FS|__GFP_WAIT))
		return 0;
	if (!nr_to_scan)
		goto out;

	mutex_lock(&qi->qi_lru_lock);
	while (!list_empty(&qi->qi_lru_list)) {
		if (nr_to_scan-- <= 0)
			break;
		dqp = list_first_entry(&qi->qi_lru_list, struct xfs_dquot,
				       q_lru);
		xfs_qm_dqreclaim_one(dqp, &dispose_list);
	}
	mutex_unlock(&qi->qi_lru_lock);

	while (!list_empty(&dispose_list)) {
		dqp = list_first_entry(&dispose_list, struct xfs_dquot, q_lru);
		list_del_init(&dqp->q_lru);
		xfs_qm_dqfree_one(dqp);
	}
out:
	return (qi->qi_lru_count / 100) * sysctl_vfs_cache_pressure;
}

/*
 * Start a transaction and write the incore superblock changes to
 * disk. flags parameter indicates which fields have changed.
 */
int
xfs_qm_write_sb_changes(
	xfs_mount_t	*mp,
	__int64_t	flags)
{
	xfs_trans_t	*tp;
	int		error;

	tp = xfs_trans_alloc(mp, XFS_TRANS_QM_SBCHANGE);
	if ((error = xfs_trans_reserve(tp, 0,
				      mp->m_sb.sb_sectsize + 128, 0,
				      0,
				      XFS_DEFAULT_LOG_COUNT))) {
		xfs_trans_cancel(tp, 0);
		return error;
	}

	xfs_mod_sb(tp, flags);
	error = xfs_trans_commit(tp, 0);

	return error;
}


/* --------------- utility functions for vnodeops ---------------- */


/*
 * Given an inode, a uid, gid and prid make sure that we have
 * allocated relevant dquot(s) on disk, and that we won't exceed inode
 * quotas by creating this file.
 * This also attaches dquot(s) to the given inode after locking it,
 * and returns the dquots corresponding to the uid and/or gid.
 *
 * in	: inode (unlocked)
 * out	: udquot, gdquot with references taken and unlocked
 */
int
xfs_qm_vop_dqalloc(
	struct xfs_inode	*ip,
	uid_t			uid,
	gid_t			gid,
	prid_t			prid,
	uint			flags,
	struct xfs_dquot	**O_udqpp,
	struct xfs_dquot	**O_gdqpp)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_dquot	*uq, *gq;
	int			error;
	uint			lockflags;

	if (!XFS_IS_QUOTA_RUNNING(mp) || !XFS_IS_QUOTA_ON(mp))
		return 0;

	lockflags = XFS_ILOCK_EXCL;
	xfs_ilock(ip, lockflags);

	if ((flags & XFS_QMOPT_INHERIT) && XFS_INHERIT_GID(ip))
		gid = ip->i_d.di_gid;

	/*
	 * Attach the dquot(s) to this inode, doing a dquot allocation
	 * if necessary. The dquot(s) will not be locked.
	 */
	if (XFS_NOT_DQATTACHED(mp, ip)) {
		error = xfs_qm_dqattach_locked(ip, XFS_QMOPT_DQALLOC);
		if (error) {
			xfs_iunlock(ip, lockflags);
			return error;
		}
	}

	uq = gq = NULL;
	if ((flags & XFS_QMOPT_UQUOTA) && XFS_IS_UQUOTA_ON(mp)) {
		if (ip->i_d.di_uid != uid) {
			/*
			 * What we need is the dquot that has this uid, and
			 * if we send the inode to dqget, the uid of the inode
			 * takes priority over what's sent in the uid argument.
			 * We must unlock inode here before calling dqget if
			 * we're not sending the inode, because otherwise
			 * we'll deadlock by doing trans_reserve while
			 * holding ilock.
			 */
			xfs_iunlock(ip, lockflags);
			if ((error = xfs_qm_dqget(mp, NULL, (xfs_dqid_t) uid,
						 XFS_DQ_USER,
						 XFS_QMOPT_DQALLOC |
						 XFS_QMOPT_DOWARN,
						 &uq))) {
				ASSERT(error != ENOENT);
				return error;
			}
			/*
			 * Get the ilock in the right order.
			 */
			xfs_dqunlock(uq);
			lockflags = XFS_ILOCK_SHARED;
			xfs_ilock(ip, lockflags);
		} else {
			/*
			 * Take an extra reference, because we'll return
			 * this to caller
			 */
			ASSERT(ip->i_udquot);
			uq = xfs_qm_dqhold(ip->i_udquot);
		}
	}
	if ((flags & XFS_QMOPT_GQUOTA) && XFS_IS_GQUOTA_ON(mp)) {
		if (ip->i_d.di_gid != gid) {
			xfs_iunlock(ip, lockflags);
			if ((error = xfs_qm_dqget(mp, NULL, (xfs_dqid_t)gid,
						 XFS_DQ_GROUP,
						 XFS_QMOPT_DQALLOC |
						 XFS_QMOPT_DOWARN,
						 &gq))) {
				if (uq)
					xfs_qm_dqrele(uq);
				ASSERT(error != ENOENT);
				return error;
			}
			xfs_dqunlock(gq);
			lockflags = XFS_ILOCK_SHARED;
			xfs_ilock(ip, lockflags);
		} else {
			ASSERT(ip->i_gdquot);
			gq = xfs_qm_dqhold(ip->i_gdquot);
		}
	} else if ((flags & XFS_QMOPT_PQUOTA) && XFS_IS_PQUOTA_ON(mp)) {
		if (xfs_get_projid(ip) != prid) {
			xfs_iunlock(ip, lockflags);
			if ((error = xfs_qm_dqget(mp, NULL, (xfs_dqid_t)prid,
						 XFS_DQ_PROJ,
						 XFS_QMOPT_DQALLOC |
						 XFS_QMOPT_DOWARN,
						 &gq))) {
				if (uq)
					xfs_qm_dqrele(uq);
				ASSERT(error != ENOENT);
				return (error);
			}
			xfs_dqunlock(gq);
			lockflags = XFS_ILOCK_SHARED;
			xfs_ilock(ip, lockflags);
		} else {
			ASSERT(ip->i_gdquot);
			gq = xfs_qm_dqhold(ip->i_gdquot);
		}
	}
	if (uq)
		trace_xfs_dquot_dqalloc(ip);

	xfs_iunlock(ip, lockflags);
	if (O_udqpp)
		*O_udqpp = uq;
	else if (uq)
		xfs_qm_dqrele(uq);
	if (O_gdqpp)
		*O_gdqpp = gq;
	else if (gq)
		xfs_qm_dqrele(gq);
	return 0;
}

/*
 * Actually transfer ownership, and do dquot modifications.
 * These were already reserved.
 */
xfs_dquot_t *
xfs_qm_vop_chown(
	xfs_trans_t	*tp,
	xfs_inode_t	*ip,
	xfs_dquot_t	**IO_olddq,
	xfs_dquot_t	*newdq)
{
	xfs_dquot_t	*prevdq;
	uint		bfield = XFS_IS_REALTIME_INODE(ip) ?
				 XFS_TRANS_DQ_RTBCOUNT : XFS_TRANS_DQ_BCOUNT;


	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	ASSERT(XFS_IS_QUOTA_RUNNING(ip->i_mount));

	/* old dquot */
	prevdq = *IO_olddq;
	ASSERT(prevdq);
	ASSERT(prevdq != newdq);

	xfs_trans_mod_dquot(tp, prevdq, bfield, -(ip->i_d.di_nblocks));
	xfs_trans_mod_dquot(tp, prevdq, XFS_TRANS_DQ_ICOUNT, -1);

	/* the sparkling new dquot */
	xfs_trans_mod_dquot(tp, newdq, bfield, ip->i_d.di_nblocks);
	xfs_trans_mod_dquot(tp, newdq, XFS_TRANS_DQ_ICOUNT, 1);

	/*
	 * Take an extra reference, because the inode is going to keep
	 * this dquot pointer even after the trans_commit.
	 */
	*IO_olddq = xfs_qm_dqhold(newdq);

	return prevdq;
}

/*
 * Quota reservations for setattr(AT_UID|AT_GID|AT_PROJID).
 */
int
xfs_qm_vop_chown_reserve(
	xfs_trans_t	*tp,
	xfs_inode_t	*ip,
	xfs_dquot_t	*udqp,
	xfs_dquot_t	*gdqp,
	uint		flags)
{
	xfs_mount_t	*mp = ip->i_mount;
	uint		delblks, blkflags, prjflags = 0;
	xfs_dquot_t	*unresudq, *unresgdq, *delblksudq, *delblksgdq;
	int		error;


	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL|XFS_ILOCK_SHARED));
	ASSERT(XFS_IS_QUOTA_RUNNING(mp));

	delblks = ip->i_delayed_blks;
	delblksudq = delblksgdq = unresudq = unresgdq = NULL;
	blkflags = XFS_IS_REALTIME_INODE(ip) ?
			XFS_QMOPT_RES_RTBLKS : XFS_QMOPT_RES_REGBLKS;

	if (XFS_IS_UQUOTA_ON(mp) && udqp &&
	    ip->i_d.di_uid != (uid_t)be32_to_cpu(udqp->q_core.d_id)) {
		delblksudq = udqp;
		/*
		 * If there are delayed allocation blocks, then we have to
		 * unreserve those from the old dquot, and add them to the
		 * new dquot.
		 */
		if (delblks) {
			ASSERT(ip->i_udquot);
			unresudq = ip->i_udquot;
		}
	}
	if (XFS_IS_OQUOTA_ON(ip->i_mount) && gdqp) {
		if (XFS_IS_PQUOTA_ON(ip->i_mount) &&
		     xfs_get_projid(ip) != be32_to_cpu(gdqp->q_core.d_id))
			prjflags = XFS_QMOPT_ENOSPC;

		if (prjflags ||
		    (XFS_IS_GQUOTA_ON(ip->i_mount) &&
		     ip->i_d.di_gid != be32_to_cpu(gdqp->q_core.d_id))) {
			delblksgdq = gdqp;
			if (delblks) {
				ASSERT(ip->i_gdquot);
				unresgdq = ip->i_gdquot;
			}
		}
	}

	if ((error = xfs_trans_reserve_quota_bydquots(tp, ip->i_mount,
				delblksudq, delblksgdq, ip->i_d.di_nblocks, 1,
				flags | blkflags | prjflags)))
		return (error);

	/*
	 * Do the delayed blks reservations/unreservations now. Since, these
	 * are done without the help of a transaction, if a reservation fails
	 * its previous reservations won't be automatically undone by trans
	 * code. So, we have to do it manually here.
	 */
	if (delblks) {
		/*
		 * Do the reservations first. Unreservation can't fail.
		 */
		ASSERT(delblksudq || delblksgdq);
		ASSERT(unresudq || unresgdq);
		if ((error = xfs_trans_reserve_quota_bydquots(NULL, ip->i_mount,
				delblksudq, delblksgdq, (xfs_qcnt_t)delblks, 0,
				flags | blkflags | prjflags)))
			return (error);
		xfs_trans_reserve_quota_bydquots(NULL, ip->i_mount,
				unresudq, unresgdq, -((xfs_qcnt_t)delblks), 0,
				blkflags);
	}

	return (0);
}

int
xfs_qm_vop_rename_dqattach(
	struct xfs_inode	**i_tab)
{
	struct xfs_mount	*mp = i_tab[0]->i_mount;
	int			i;

	if (!XFS_IS_QUOTA_RUNNING(mp) || !XFS_IS_QUOTA_ON(mp))
		return 0;

	for (i = 0; (i < 4 && i_tab[i]); i++) {
		struct xfs_inode	*ip = i_tab[i];
		int			error;

		/*
		 * Watch out for duplicate entries in the table.
		 */
		if (i == 0 || ip != i_tab[i-1]) {
			if (XFS_NOT_DQATTACHED(mp, ip)) {
				error = xfs_qm_dqattach(ip, 0);
				if (error)
					return error;
			}
		}
	}
	return 0;
}

void
xfs_qm_vop_create_dqattach(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	struct xfs_dquot	*udqp,
	struct xfs_dquot	*gdqp)
{
	struct xfs_mount	*mp = tp->t_mountp;

	if (!XFS_IS_QUOTA_RUNNING(mp) || !XFS_IS_QUOTA_ON(mp))
		return;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	ASSERT(XFS_IS_QUOTA_RUNNING(mp));

	if (udqp) {
		ASSERT(ip->i_udquot == NULL);
		ASSERT(XFS_IS_UQUOTA_ON(mp));
		ASSERT(ip->i_d.di_uid == be32_to_cpu(udqp->q_core.d_id));

		ip->i_udquot = xfs_qm_dqhold(udqp);
		xfs_trans_mod_dquot(tp, udqp, XFS_TRANS_DQ_ICOUNT, 1);
	}
	if (gdqp) {
		ASSERT(ip->i_gdquot == NULL);
		ASSERT(XFS_IS_OQUOTA_ON(mp));
		ASSERT((XFS_IS_GQUOTA_ON(mp) ?
			ip->i_d.di_gid : xfs_get_projid(ip)) ==
				be32_to_cpu(gdqp->q_core.d_id));

		ip->i_gdquot = xfs_qm_dqhold(gdqp);
		xfs_trans_mod_dquot(tp, gdqp, XFS_TRANS_DQ_ICOUNT, 1);
	}
}
