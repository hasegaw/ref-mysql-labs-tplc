/*
   Copyright (c) 2005, 2013, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#define DBTUP_C
#define DBTUP_SCAN_CPP
#include "Dbtup.hpp"
#include <signaldata/AccScan.hpp>
#include <signaldata/NextScan.hpp>
#include <signaldata/AccLock.hpp>
#include <md5_hash.hpp>

#define JAM_FILE_ID 408


#ifdef VM_TRACE
#define dbg(x) globalSignalLoggers.log x
#else
#define dbg(x)
#endif

void
Dbtup::execACC_SCANREQ(Signal* signal)
{
  jamEntry();
  const AccScanReq reqCopy = *(const AccScanReq*)signal->getDataPtr();
  const AccScanReq* const req = &reqCopy;
  ScanOpPtr scanPtr;
  scanPtr.i = RNIL;
  do {
    // find table and fragment
    TablerecPtr tablePtr;
    tablePtr.i = req->tableId;
    ptrCheckGuard(tablePtr, cnoOfTablerec, tablerec);
    FragrecordPtr fragPtr;
    Uint32 fragId = req->fragmentNo;
    fragPtr.i = RNIL;
    getFragmentrec(fragPtr, fragId, tablePtr.p);
    ndbrequire(fragPtr.i != RNIL);
    Fragrecord& frag = *fragPtr.p;
    // flags
    Uint32 bits = 0;
    
    if (AccScanReq::getLcpScanFlag(req->requestInfo))
    {
      jam();
      bits |= ScanOp::SCAN_LCP;
      c_scanOpPool.getPtr(scanPtr, c_lcp_scan_op);
      ndbrequire(scanPtr.p->m_fragPtrI == fragPtr.i);
      ndbrequire(scanPtr.p->m_state == ScanOp::First);
    }
    else
    {
      // seize from pool and link to per-fragment list
      LocalDLList<ScanOp> list(c_scanOpPool, frag.m_scanList);
      if (! list.seizeFirst(scanPtr)) {
	jam();
	break;
      }
      new (scanPtr.p) ScanOp;
    }

    if (!AccScanReq::getNoDiskScanFlag(req->requestInfo)
        && tablePtr.p->m_no_of_disk_attributes)
    {
      bits |= ScanOp::SCAN_DD;
    }
      
    bool mm = (bits & ScanOp::SCAN_DD);
    if ((tablePtr.p->m_attributes[mm].m_no_of_varsize +
         tablePtr.p->m_attributes[mm].m_no_of_dynamic) > 0) 
    {
      if (bits & ScanOp::SCAN_DD)
      {
        // only dd scan varsize pages
        // mm always has a fixed part
        bits |= ScanOp::SCAN_VS;
      }
    }

    if (! AccScanReq::getReadCommittedFlag(req->requestInfo)) 
    {
      if (AccScanReq::getLockMode(req->requestInfo) == 0)
        bits |= ScanOp::SCAN_LOCK_SH;
      else
        bits |= ScanOp::SCAN_LOCK_EX;
    }

    if (AccScanReq::getNRScanFlag(req->requestInfo))
    {
      jam();
      bits |= ScanOp::SCAN_NR;
      scanPtr.p->m_endPage = req->maxPage;
      if (req->maxPage != RNIL && req->maxPage > frag.m_max_page_no)
      {
        ndbout_c("%u %u endPage: %u (noOfPages: %u maxPage: %u)", 
                 tablePtr.i, fragId,
                 req->maxPage, fragPtr.p->noOfPages,
                 fragPtr.p->m_max_page_no);
      }
    }
    else
    {
      jam();
      scanPtr.p->m_endPage = RNIL;
    }

    if (AccScanReq::getLcpScanFlag(req->requestInfo))
    {
      jam();
      ndbrequire((bits & ScanOp::SCAN_DD) == 0);
      ndbrequire((bits & ScanOp::SCAN_LOCK) == 0);
    }

    if (bits & ScanOp::SCAN_VS)
    {
      ndbrequire((bits & ScanOp::SCAN_NR) == 0);
      ndbrequire((bits & ScanOp::SCAN_LCP) == 0);
    }
    
    // set up scan op
    ScanOp& scan = *scanPtr.p;
    scan.m_state = ScanOp::First;
    scan.m_bits = bits;
    scan.m_userPtr = req->senderData;
    scan.m_userRef = req->senderRef;
    scan.m_tableId = tablePtr.i;
    scan.m_fragId = frag.fragmentId;
    scan.m_fragPtrI = fragPtr.i;
    scan.m_transId1 = req->transId1;
    scan.m_transId2 = req->transId2;
    scan.m_savePointId = req->savePointId;

    // conf
    AccScanConf* const conf = (AccScanConf*)signal->getDataPtrSend();
    conf->scanPtr = req->senderData;
    conf->accPtr = scanPtr.i;
    conf->flag = AccScanConf::ZNOT_EMPTY_FRAGMENT;
    sendSignal(req->senderRef, GSN_ACC_SCANCONF,
        signal, AccScanConf::SignalLength, JBB);
    return;
  } while (0);
  if (scanPtr.i != RNIL) {
    jam();
    releaseScanOp(scanPtr);
  }
  // LQH does not handle REF
  signal->theData[0] = 0x313;
  sendSignal(req->senderRef, GSN_ACC_SCANREF, signal, 1, JBB);
}

void
Dbtup::execNEXT_SCANREQ(Signal* signal)
{
  jamEntry();
  const NextScanReq reqCopy = *(const NextScanReq*)signal->getDataPtr();
  const NextScanReq* const req = &reqCopy;
  ScanOpPtr scanPtr;
  c_scanOpPool.getPtr(scanPtr, req->accPtr);
  ScanOp& scan = *scanPtr.p;
  switch (req->scanFlag) {
  case NextScanReq::ZSCAN_NEXT:
    jam();
    break;
  case NextScanReq::ZSCAN_NEXT_COMMIT:
    jam();
  case NextScanReq::ZSCAN_COMMIT:
    jam();
    if ((scan.m_bits & ScanOp::SCAN_LOCK) != 0) {
      jam();
      AccLockReq* const lockReq = (AccLockReq*)signal->getDataPtrSend();
      lockReq->returnCode = RNIL;
      lockReq->requestInfo = AccLockReq::Unlock;
      lockReq->accOpPtr = req->accOperationPtr;
      EXECUTE_DIRECT(DBACC, GSN_ACC_LOCKREQ,
          signal, AccLockReq::UndoSignalLength);
      jamEntry();
      ndbrequire(lockReq->returnCode == AccLockReq::Success);
      removeAccLockOp(scan, req->accOperationPtr);
    }
    if (req->scanFlag == NextScanReq::ZSCAN_COMMIT) {
      NextScanConf* const conf = (NextScanConf*)signal->getDataPtrSend();
      conf->scanPtr = scan.m_userPtr;
      unsigned signalLength = 1;
      sendSignal(scanPtr.p->m_userRef, GSN_NEXT_SCANCONF,
		 signal, signalLength, JBB);
      return;
    }
    break;
  case NextScanReq::ZSCAN_CLOSE:
    jam();
    if (scan.m_bits & ScanOp::SCAN_LOCK_WAIT) {
      jam();
      ndbrequire(scan.m_accLockOp != RNIL);
      // use ACC_ABORTCONF to flush out any reply in job buffer
      AccLockReq* const lockReq = (AccLockReq*)signal->getDataPtrSend();
      lockReq->returnCode = RNIL;
      lockReq->requestInfo = AccLockReq::AbortWithConf;
      lockReq->accOpPtr = scan.m_accLockOp;
      EXECUTE_DIRECT(DBACC, GSN_ACC_LOCKREQ,
		     signal, AccLockReq::UndoSignalLength);
      jamEntry();
      ndbrequire(lockReq->returnCode == AccLockReq::Success);
      scan.m_state = ScanOp::Aborting;
      return;
    }
    if (scan.m_state == ScanOp::Locked) {
      jam();
      ndbrequire(scan.m_accLockOp != RNIL);
      AccLockReq* const lockReq = (AccLockReq*)signal->getDataPtrSend();
      lockReq->returnCode = RNIL;
      lockReq->requestInfo = AccLockReq::Abort;
      lockReq->accOpPtr = scan.m_accLockOp;
      EXECUTE_DIRECT(DBACC, GSN_ACC_LOCKREQ,
		     signal, AccLockReq::UndoSignalLength);
      jamEntry();
      ndbrequire(lockReq->returnCode == AccLockReq::Success);
      scan.m_accLockOp = RNIL;
    }
    scan.m_state = ScanOp::Aborting;
    scanClose(signal, scanPtr);
    return;
  case NextScanReq::ZSCAN_NEXT_ABORT:
    jam();
  default:
    jam();
    ndbrequire(false);
    break;
  }
  // start looking for next scan result
  AccCheckScan* checkReq = (AccCheckScan*)signal->getDataPtrSend();
  checkReq->accPtr = scanPtr.i;
  checkReq->checkLcpStop = AccCheckScan::ZNOT_CHECK_LCP_STOP;
  EXECUTE_DIRECT(DBTUP, GSN_ACC_CHECK_SCAN, signal, AccCheckScan::SignalLength);
  jamEntry();
}

void
Dbtup::execACC_CHECK_SCAN(Signal* signal)
{
  jamEntry();
  const AccCheckScan reqCopy = *(const AccCheckScan*)signal->getDataPtr();
  const AccCheckScan* const req = &reqCopy;
  ScanOpPtr scanPtr;
  c_scanOpPool.getPtr(scanPtr, req->accPtr);
  ScanOp& scan = *scanPtr.p;
  // fragment
  FragrecordPtr fragPtr;
  fragPtr.i = scan.m_fragPtrI;
  ptrCheckGuard(fragPtr, cnoOfFragrec, fragrecord);
  Fragrecord& frag = *fragPtr.p;
  if (req->checkLcpStop == AccCheckScan::ZCHECK_LCP_STOP) {
    jam();
    signal->theData[0] = scan.m_userPtr;
    signal->theData[1] = true;
    EXECUTE_DIRECT(DBLQH, GSN_CHECK_LCP_STOP, signal, 2);
    jamEntry();
    return;
  }
  if (scan.m_bits & ScanOp::SCAN_LOCK_WAIT) {
    jam();
    // LQH asks if we are waiting for lock and we tell it to ask again
    NextScanConf* const conf = (NextScanConf*)signal->getDataPtrSend();
    conf->scanPtr = scan.m_userPtr;
    conf->accOperationPtr = RNIL;       // no tuple returned
    conf->fragId = frag.fragmentId;
    unsigned signalLength = 3;
    // if TC has ordered scan close, it will be detected here
    sendSignal(scan.m_userRef, GSN_NEXT_SCANCONF,
        signal, signalLength, JBB);
    return;     // stop
  }

  const bool lcp = (scan.m_bits & ScanOp::SCAN_LCP);

  if (lcp && ! fragPtr.p->m_lcp_keep_list_head.isNull())
  {
    jam();
    /**
     * Handle lcp keep list already here
     *   So that scan state is not alterer
     *   if lcp_keep rows are found in ScanOp::First
     */
    handle_lcp_keep(signal, fragPtr.p, scanPtr.p);
    return;
  }

  if (scan.m_state == ScanOp::First) {
    jam();
    scanFirst(signal, scanPtr);
  }
  if (scan.m_state == ScanOp::Next) {
    jam();
    bool immediate = scanNext(signal, scanPtr);
    if (! immediate) {
      jam();
      // time-slicing via TUP or PGMAN
      return;
    }
  }
  scanReply(signal, scanPtr);
}

void
Dbtup::scanReply(Signal* signal, ScanOpPtr scanPtr)
{
  ScanOp& scan = *scanPtr.p;
  FragrecordPtr fragPtr;
  fragPtr.i = scan.m_fragPtrI;
  ptrCheckGuard(fragPtr, cnoOfFragrec, fragrecord);
  Fragrecord& frag = *fragPtr.p;
  // for reading tuple key in Current state
  Uint32* pkData = (Uint32*)c_dataBuffer;
  unsigned pkSize = 0;
  if (scan.m_state == ScanOp::Current) {
    // found an entry to return
    jam();
    ndbrequire(scan.m_accLockOp == RNIL);
    if (scan.m_bits & ScanOp::SCAN_LOCK) {
      jam();
      // read tuple key - use TUX routine
      const ScanPos& pos = scan.m_scanPos;
      const Local_key& key_mm = pos.m_key_mm;
      int ret = tuxReadPk(fragPtr.i, pos.m_realpid_mm, key_mm.m_page_idx,
			  pkData, true);
      ndbrequire(ret > 0);
      pkSize = ret;
      dbg((DBTUP, "PK size=%d data=%08x", pkSize, pkData[0]));
      // get read lock or exclusive lock
      AccLockReq* const lockReq = (AccLockReq*)signal->getDataPtrSend();
      lockReq->returnCode = RNIL;
      lockReq->requestInfo = (scan.m_bits & ScanOp::SCAN_LOCK_SH) ?
        AccLockReq::LockShared : AccLockReq::LockExclusive;
      lockReq->accOpPtr = RNIL;
      lockReq->userPtr = scanPtr.i;
      lockReq->userRef = reference();
      lockReq->tableId = scan.m_tableId;
      lockReq->fragId = frag.fragmentId;
      lockReq->fragPtrI = RNIL; // no cached frag ptr yet
      lockReq->hashValue = md5_hash((Uint64*)pkData, pkSize);
      lockReq->page_id = key_mm.m_page_no;
      lockReq->page_idx = key_mm.m_page_idx;
      lockReq->transId1 = scan.m_transId1;
      lockReq->transId2 = scan.m_transId2;
      EXECUTE_DIRECT(DBACC, GSN_ACC_LOCKREQ,
          signal, AccLockReq::LockSignalLength);
      jamEntry();
      switch (lockReq->returnCode) {
      case AccLockReq::Success:
        jam();
        scan.m_state = ScanOp::Locked;
        scan.m_accLockOp = lockReq->accOpPtr;
        break;
      case AccLockReq::IsBlocked:
        jam();
        // normal lock wait
        scan.m_state = ScanOp::Blocked;
        scan.m_bits |= ScanOp::SCAN_LOCK_WAIT;
        scan.m_accLockOp = lockReq->accOpPtr;
        // LQH will wake us up
        signal->theData[0] = scan.m_userPtr;
        signal->theData[1] = true;
        EXECUTE_DIRECT(DBLQH, GSN_CHECK_LCP_STOP, signal, 2);
        jamEntry();
        return;
        break;
      case AccLockReq::Refused:
        jam();
        // we cannot see deleted tuple (assert only)
        ndbassert(false);
        // skip it
        scan.m_state = ScanOp::Next;
        signal->theData[0] = scan.m_userPtr;
        signal->theData[1] = true;
        EXECUTE_DIRECT(DBLQH, GSN_CHECK_LCP_STOP, signal, 2);
        jamEntry();
        return;
        break;
      case AccLockReq::NoFreeOp:
        jam();
        // max ops should depend on max scans (assert only)
        ndbassert(false);
        // stay in Current state
        scan.m_state = ScanOp::Current;
        signal->theData[0] = scan.m_userPtr;
        signal->theData[1] = true;
        EXECUTE_DIRECT(DBLQH, GSN_CHECK_LCP_STOP, signal, 2);
        jamEntry();
        return;
        break;
      default:
        ndbrequire(false);
        break;
      }
    } else {
      scan.m_state = ScanOp::Locked;
    }
  } 

  if (scan.m_state == ScanOp::Locked) {
    // we have lock or do not need one
    jam();
    // conf signal
    NextScanConf* const conf = (NextScanConf*)signal->getDataPtrSend();
    conf->scanPtr = scan.m_userPtr;
    // the lock is passed to LQH
    Uint32 accLockOp = scan.m_accLockOp;
    if (accLockOp != RNIL) {
      scan.m_accLockOp = RNIL;
      // remember it until LQH unlocks it
      addAccLockOp(scan, accLockOp);
    } else {
      ndbrequire(! (scan.m_bits & ScanOp::SCAN_LOCK));
      // operation RNIL in LQH would signal no tuple returned
      accLockOp = (Uint32)-1;
    }
    const ScanPos& pos = scan.m_scanPos;
    conf->accOperationPtr = accLockOp;
    conf->fragId = frag.fragmentId;
    conf->localKey[0] = pos.m_key_mm.m_page_no;
    conf->localKey[1] = pos.m_key_mm.m_page_idx;
    unsigned signalLength = 5;
    if (scan.m_bits & ScanOp::SCAN_LOCK) {
      sendSignal(scan.m_userRef, GSN_NEXT_SCANCONF,
          signal, signalLength, JBB);
    } else {
      Uint32 blockNo = refToMain(scan.m_userRef);
      EXECUTE_DIRECT(blockNo, GSN_NEXT_SCANCONF, signal, signalLength);
      jamEntry();
    }
    // next time look for next entry
    scan.m_state = ScanOp::Next;
    return;
  }
  if (scan.m_state == ScanOp::Last ||
      scan.m_state == ScanOp::Invalid) {
    jam();
    NextScanConf* const conf = (NextScanConf*)signal->getDataPtrSend();
    conf->scanPtr = scan.m_userPtr;
    conf->accOperationPtr = RNIL;
    conf->fragId = RNIL;
    unsigned signalLength = 3;
    sendSignal(scanPtr.p->m_userRef, GSN_NEXT_SCANCONF,
        signal, signalLength, JBB);
    return;
  }
  ndbrequire(false);
}

/*
 * Lock succeeded (after delay) in ACC.  If the lock is for current
 * entry, set state to Locked.  If the lock is for an entry we were
 * moved away from, simply unlock it.  Finally, if we are closing the
 * scan, do nothing since we have already sent an abort request.
 */
void
Dbtup::execACCKEYCONF(Signal* signal)
{
  jamEntry();
  ScanOpPtr scanPtr;
  scanPtr.i = signal->theData[0];

  Uint32 localKey1 = signal->theData[3];
  Uint32 localKey2 = signal->theData[4];
  Local_key tmp;
  tmp.m_page_no = localKey1;
  tmp.m_page_idx = localKey2;

  c_scanOpPool.getPtr(scanPtr);
  ScanOp& scan = *scanPtr.p;
  ndbrequire(scan.m_bits & ScanOp::SCAN_LOCK_WAIT && scan.m_accLockOp != RNIL);
  scan.m_bits &= ~ ScanOp::SCAN_LOCK_WAIT;
  if (scan.m_state == ScanOp::Blocked) {
    // the lock wait was for current entry
    jam();

    if (likely(scan.m_scanPos.m_key_mm.m_page_no == tmp.m_page_no &&
               scan.m_scanPos.m_key_mm.m_page_idx == tmp.m_page_idx))
    {
      jam();
      scan.m_state = ScanOp::Locked;
      // LQH has the ball
      return;
    }
    else
    {
      jam();
      /**
       * This means that there was DEL/INS on rowid that we tried to lock
       *   and the primary key that was previously located on this rowid
       *   (scanPos.m_key_mm) has moved.
       *   (DBACC keeps of track of primary keys)
       *
       * We don't care about the primary keys, but is interested in ROWID
       *   so rescan this position.
       *   Which is implemented by using execACCKEYREF...
       */
      ndbout << "execACCKEYCONF "
             << scan.m_scanPos.m_key_mm
             << " != " << tmp << " ";
      scan.m_bits |= ScanOp::SCAN_LOCK_WAIT;
      execACCKEYREF(signal);
      return;
    }
  }

  if (scan.m_state != ScanOp::Aborting) {
    // we were moved, release lock
    jam();
    AccLockReq* const lockReq = (AccLockReq*)signal->getDataPtrSend();
    lockReq->returnCode = RNIL;
    lockReq->requestInfo = AccLockReq::Abort;
    lockReq->accOpPtr = scan.m_accLockOp;
    EXECUTE_DIRECT(DBACC, GSN_ACC_LOCKREQ, signal, AccLockReq::UndoSignalLength);
    jamEntry();
    ndbrequire(lockReq->returnCode == AccLockReq::Success);
    scan.m_accLockOp = RNIL;
    // LQH has the ball
    return;
  }
  // lose the lock
  scan.m_accLockOp = RNIL;
  // continue at ACC_ABORTCONF
}

/*
 * Lock failed (after delay) in ACC.  Probably means somebody ahead of
 * us in lock queue deleted the tuple.
 */
void
Dbtup::execACCKEYREF(Signal* signal)
{
  jamEntry();
  ScanOpPtr scanPtr;
  scanPtr.i = signal->theData[0];
  c_scanOpPool.getPtr(scanPtr);
  ScanOp& scan = *scanPtr.p;
  ndbrequire(scan.m_bits & ScanOp::SCAN_LOCK_WAIT && scan.m_accLockOp != RNIL);
  scan.m_bits &= ~ ScanOp::SCAN_LOCK_WAIT;
  if (scan.m_state != ScanOp::Aborting) {
    jam();
    // release the operation
    AccLockReq* const lockReq = (AccLockReq*)signal->getDataPtrSend();
    lockReq->returnCode = RNIL;
    lockReq->requestInfo = AccLockReq::Abort;
    lockReq->accOpPtr = scan.m_accLockOp;
    EXECUTE_DIRECT(DBACC, GSN_ACC_LOCKREQ, signal, AccLockReq::UndoSignalLength);
    jamEntry();
    ndbrequire(lockReq->returnCode == AccLockReq::Success);
    scan.m_accLockOp = RNIL;
    // scan position should already have been moved (assert only)
    if (scan.m_state == ScanOp::Blocked) {
      jam();
      //ndbassert(false);
      if (scan.m_bits & ScanOp::SCAN_NR)
      {
	jam();
	scan.m_state = ScanOp::Next;
	scan.m_scanPos.m_get = ScanPos::Get_tuple;
	ndbout_c("Ignoring scan.m_state == ScanOp::Blocked, refetch");
      }
      else
      {
	jam();
	scan.m_state = ScanOp::Next;
	ndbout_c("Ignoring scan.m_state == ScanOp::Blocked");
      }
    }
    // LQH has the ball
    return;
  }
  // lose the lock
  scan.m_accLockOp = RNIL;
  // continue at ACC_ABORTCONF
}

/*
 * Received when scan is closing.  This signal arrives after any
 * ACCKEYCON or ACCKEYREF which may have been in job buffer.
 */
void
Dbtup::execACC_ABORTCONF(Signal* signal)
{
  jamEntry();
  ScanOpPtr scanPtr;
  scanPtr.i = signal->theData[0];
  c_scanOpPool.getPtr(scanPtr);
  ScanOp& scan = *scanPtr.p;
  ndbrequire(scan.m_state == ScanOp::Aborting);
  // most likely we are still in lock wait
  if (scan.m_bits & ScanOp::SCAN_LOCK_WAIT) {
    jam();
    scan.m_bits &= ~ ScanOp::SCAN_LOCK_WAIT;
    scan.m_accLockOp = RNIL;
  }
  scanClose(signal, scanPtr);
}

void
Dbtup::scanFirst(Signal*, ScanOpPtr scanPtr)
{
  ScanOp& scan = *scanPtr.p;
  ScanPos& pos = scan.m_scanPos;
  Local_key& key = pos.m_key;
  const Uint32 bits = scan.m_bits;
  // fragment
  FragrecordPtr fragPtr;
  fragPtr.i = scan.m_fragPtrI;
  ptrCheckGuard(fragPtr, cnoOfFragrec, fragrecord);
  Fragrecord& frag = *fragPtr.p;

  if (bits & ScanOp::SCAN_NR)
  { 
    if (scan.m_endPage == 0 && frag.m_max_page_no == 0)
    {
      jam();
      scan.m_state = ScanOp::Last;
      return;
    }
  }
  else if (frag.noOfPages == 0)
  {
    jam();
    scan.m_state = ScanOp::Last;
    return;
  }

  if (! (bits & ScanOp::SCAN_DD)) {
    key.m_file_no = ZNIL;
    key.m_page_no = 0;
    pos.m_get = ScanPos::Get_page_mm;
    // for MM scan real page id is cached for efficiency
    pos.m_realpid_mm = RNIL;
  } else {
    Disk_alloc_info& alloc = frag.m_disk_alloc_info;
    // for now must check disk part explicitly
    if (alloc.m_extent_list.isEmpty()) {
      jam();
      scan.m_state = ScanOp::Last;
      return;
    }
    pos.m_extent_info_ptr_i = alloc.m_extent_list.getFirst();
    Extent_info* ext = c_extent_pool.getPtr(pos.m_extent_info_ptr_i);
    key.m_file_no = ext->m_key.m_file_no;
    key.m_page_no = ext->m_first_page_no;
    pos.m_get = ScanPos::Get_page_dd;
  }
  key.m_page_idx = ((bits & ScanOp::SCAN_VS) == 0) ? 0 : 1;
  // let scanNext() do the work
  scan.m_state = ScanOp::Next;
}

bool
Dbtup::scanNext(Signal* signal, ScanOpPtr scanPtr)
{
  ScanOp& scan = *scanPtr.p;
  ScanPos& pos = scan.m_scanPos;
  Local_key& key = pos.m_key;
  const Uint32 bits = scan.m_bits;
  // table
  TablerecPtr tablePtr;
  tablePtr.i = scan.m_tableId;
  ptrCheckGuard(tablePtr, cnoOfTablerec, tablerec);
  Tablerec& table = *tablePtr.p;
  // fragment
  FragrecordPtr fragPtr;
  fragPtr.i = scan.m_fragPtrI;
  ptrCheckGuard(fragPtr, cnoOfFragrec, fragrecord);
  Fragrecord& frag = *fragPtr.p;
  // tuple found
  Tuple_header* th = 0;
  Uint32 thbits = 0;
  Uint32 loop_count = 0;
  Uint32 scanGCI = scanPtr.p->m_scanGCI;
  Uint32 foundGCI;
 
  const bool mm = (bits & ScanOp::SCAN_DD);
  const bool lcp = (bits & ScanOp::SCAN_LCP);

  const Uint32 size = ((bits & ScanOp::SCAN_VS) == 0) ?
    table.m_offsets[mm].m_fix_header_size : 1;
  const Uint32 first = ((bits & ScanOp::SCAN_VS) == 0) ? 0 : 1;

  if (lcp && ! fragPtr.p->m_lcp_keep_list_head.isNull())
  {
    jam();
    /**
     * Handle lcp keep list here to, due to scanCont
     */
    handle_lcp_keep(signal, fragPtr.p, scanPtr.p);
    return false;
  }

  switch(pos.m_get){
  case ScanPos::Get_next_tuple:
    jam();
    key.m_page_idx += size;
    // fall through
  case ScanPos::Get_tuple:
    jam();
    /**
     * We need to refetch page after timeslice
     */
    pos.m_get = ScanPos::Get_page;
    pos.m_realpid_mm = RNIL;
    break;
  default:
    break;
  }
  
  while (true) {
    switch (pos.m_get) {
    case ScanPos::Get_next_page:
      // move to next page
      jam();
      {
        if (! (bits & ScanOp::SCAN_DD))
          pos.m_get = ScanPos::Get_next_page_mm;
        else
          pos.m_get = ScanPos::Get_next_page_dd;
      }
      continue;
    case ScanPos::Get_page:
      // get real page
      jam();
      {
        if (! (bits & ScanOp::SCAN_DD))
          pos.m_get = ScanPos::Get_page_mm;
        else
          pos.m_get = ScanPos::Get_page_dd;
      }
      continue;
    case ScanPos::Get_next_page_mm:
      // move to next logical TUP page
      jam();
      {
        key.m_page_no++;
        if (key.m_page_no >= frag.m_max_page_no) {
          jam();

          if ((bits & ScanOp::SCAN_NR) && (scan.m_endPage != RNIL))
          {
            jam();
            if (key.m_page_no < scan.m_endPage)
            {
              jam();
              ndbout_c("scanning page %u", key.m_page_no);
              goto cont;
            }
          }
          // no more pages, scan ends
          pos.m_get = ScanPos::Get_undef;
          scan.m_state = ScanOp::Last;
          return true;
        }
    cont:
        key.m_page_idx = first;
        pos.m_get = ScanPos::Get_page_mm;
        // clear cached value
        pos.m_realpid_mm = RNIL;
      }
      /*FALLTHRU*/
    case ScanPos::Get_page_mm:
      // get TUP real page
      jam();
      {
        if (pos.m_realpid_mm == RNIL) {
          jam();
          pos.m_realpid_mm = getRealpidCheck(fragPtr.p, key.m_page_no);
          
          if (pos.m_realpid_mm == RNIL)
          {
            jam();
            if (bits & ScanOp::SCAN_NR)
            {
              jam();
              goto nopage;
            }
            pos.m_get = ScanPos::Get_next_page_mm;
            break; // incr loop count
          }
        }
        PagePtr pagePtr;
	c_page_pool.getPtr(pagePtr, pos.m_realpid_mm);

    nopage:
        pos.m_page = pagePtr.p;
        pos.m_get = ScanPos::Get_tuple;
      }
      continue;
    case ScanPos::Get_next_page_dd:
      // move to next disk page
      jam();
      {
        Disk_alloc_info& alloc = frag.m_disk_alloc_info;
        Local_fragment_extent_list list(c_extent_pool, alloc.m_extent_list);
        Ptr<Extent_info> ext_ptr;
        c_extent_pool.getPtr(ext_ptr, pos.m_extent_info_ptr_i);
        Extent_info* ext = ext_ptr.p;
        key.m_page_no++;
        if (key.m_page_no >= ext->m_first_page_no + alloc.m_extent_size) {
          // no more pages in this extent
          jam();
          if (! list.next(ext_ptr)) {
            // no more extents, scan ends
            jam();
            pos.m_get = ScanPos::Get_undef;
            scan.m_state = ScanOp::Last;
            return true;
          } else {
            // move to next extent
            jam();
            pos.m_extent_info_ptr_i = ext_ptr.i;
            ext = c_extent_pool.getPtr(pos.m_extent_info_ptr_i);
            key.m_file_no = ext->m_key.m_file_no;
            key.m_page_no = ext->m_first_page_no;
          }
        }
        key.m_page_idx = first;
        pos.m_get = ScanPos::Get_page_dd;
        /*
          read ahead for scan in disk order
          do read ahead every 8:th page
        */
        if ((bits & ScanOp::SCAN_DD) &&
            (((key.m_page_no - ext->m_first_page_no) & 7) == 0))
        {
          jam();
          // initialize PGMAN request
          Page_cache_client::Request preq;
          preq.m_page = pos.m_key;
          preq.m_callback = TheNULLCallback;

          // set maximum read ahead
          Uint32 read_ahead = m_max_page_read_ahead;

          while (true)
          {
            // prepare page read ahead in current extent
            Uint32 page_no = preq.m_page.m_page_no;
            Uint32 page_no_limit = page_no + read_ahead;
            Uint32 limit = ext->m_first_page_no + alloc.m_extent_size;
            if (page_no_limit > limit)
            {
              jam();
              // read ahead crosses extent, set limit for this extent
              read_ahead = page_no_limit - limit;
              page_no_limit = limit;
              // and make sure we only read one extra extent next time around
              if (read_ahead > alloc.m_extent_size)
                read_ahead = alloc.m_extent_size;
            }
            else
            {
              jam();
              read_ahead = 0; // no more to read ahead after this
            }
            // do read ahead pages for this extent
            while (page_no < page_no_limit)
            {
              // page request to PGMAN
              jam();
              preq.m_page.m_page_no = page_no;
              int flags = 0;
              // ignore result
              Page_cache_client pgman(this, c_pgman);
              pgman.get_page(signal, preq, flags);
              m_pgman_ptr = pgman.m_ptr;
              jamEntry();
              page_no++;
            }
            if (!read_ahead || !list.next(ext_ptr))
            {
              // no more extents after this or read ahead done
              jam();
              break;
            }
            // move to next extent and initialize PGMAN request accordingly
            Extent_info* ext = c_extent_pool.getPtr(ext_ptr.i);
            preq.m_page.m_file_no = ext->m_key.m_file_no;
            preq.m_page.m_page_no = ext->m_first_page_no;
          }
        } // if ScanOp::SCAN_DD read ahead
      }
      /*FALLTHRU*/
    case ScanPos::Get_page_dd:
      // get global page in PGMAN cache
      jam();
      {
        // check if page is un-allocated or empty
	if (likely(! (bits & ScanOp::SCAN_NR)))
	{
          D("Tablespace_client - scanNext");
	  Tablespace_client tsman(signal, this, c_tsman,
				  frag.fragTableId, 
				  frag.fragmentId, 
				  frag.m_tablespace_id);
	  unsigned uncommitted, committed;
	  uncommitted = committed = ~(unsigned)0;
	  int ret = tsman.get_page_free_bits(&key, &uncommitted, &committed);
	  ndbrequire(ret == 0);
	  if (committed == 0 && uncommitted == 0) {
	    // skip empty page
	    jam();
	    pos.m_get = ScanPos::Get_next_page_dd;
	    break; // incr loop count
	  }
	}
        // page request to PGMAN
        Page_cache_client::Request preq;
        preq.m_page = pos.m_key;
        preq.m_callback.m_callbackData = scanPtr.i;
        preq.m_callback.m_callbackFunction =
          safe_cast(&Dbtup::disk_page_tup_scan_callback);
        int flags = 0;
        Page_cache_client pgman(this, c_pgman);
        int res = pgman.get_page(signal, preq, flags);
        m_pgman_ptr = pgman.m_ptr;
        jamEntry();
        if (res == 0) {
          jam();
          // request queued
          pos.m_get = ScanPos::Get_tuple;
          return false;
        }
        ndbrequire(res > 0);
        pos.m_page = (Page*)m_pgman_ptr.p;
      }
      pos.m_get = ScanPos::Get_tuple;
      continue;
      // get tuple
      // move to next tuple
    case ScanPos::Get_next_tuple:
      // move to next fixed size tuple
      jam();
      {
        key.m_page_idx += size;
        pos.m_get = ScanPos::Get_tuple;
      }
      /*FALLTHRU*/
    case ScanPos::Get_tuple:
      // get fixed size tuple
      jam();
      if ((bits & ScanOp::SCAN_VS) == 0)
      {
        Fix_page* page = (Fix_page*)pos.m_page;
        if (key.m_page_idx + size <= Fix_page::DATA_WORDS) 
	{
	  pos.m_get = ScanPos::Get_next_tuple;
#ifdef VM_TRACE
          if (! (bits & ScanOp::SCAN_DD))
          {
            Uint32 realpid = getRealpidCheck(fragPtr.p, key.m_page_no);
            ndbassert(pos.m_realpid_mm == realpid);
          }
#endif
          th = (Tuple_header*)&page->m_data[key.m_page_idx];
	  
	  if (likely(! (bits & ScanOp::SCAN_NR)))
	  {
	    jam();
            thbits = th->m_header_bits;
	    if (! (thbits & Tuple_header::FREE))
	    {
              goto found_tuple;
	    } 
	  }
	  else
	  {
            if (pos.m_realpid_mm == RNIL)
            {
              jam();
              foundGCI = 0;
              goto found_deleted_rowid;
            }
            thbits = th->m_header_bits;
	    if ((foundGCI = *th->get_mm_gci(tablePtr.p)) > scanGCI ||
                foundGCI == 0)
	    {
	      if (! (thbits & Tuple_header::FREE))
	      {
		jam();
		goto found_tuple;
	      }
	      else
	      {
		goto found_deleted_rowid;
	      }
	    }
	    else if (thbits != Fix_page::FREE_RECORD && 
		     th->m_operation_ptr_i != RNIL)
	    {
	      jam();
	      goto found_tuple; // Locked tuple...
	      // skip free tuple
	    }
	  }
        } else {
          jam();
          // no more tuples on this page
          pos.m_get = ScanPos::Get_next_page;
        }
      }
      else
      {
        jam();
        Var_page * page = (Var_page*)pos.m_page;
        if (key.m_page_idx < page->high_index)
        {
          jam();
          pos.m_get = ScanPos::Get_next_tuple;
          if (!page->is_free(key.m_page_idx))
          {
            th = (Tuple_header*)page->get_ptr(key.m_page_idx);
            thbits = th->m_header_bits;
            goto found_tuple;
          }
        }
        else
        {
          jam();
          // no more tuples on this page
          pos.m_get = ScanPos::Get_next_page;
          break;
        }
      }
      break; // incr loop count
  found_tuple:
      // found possible tuple to return
      jam();
      {
        // caller has already set pos.m_get to next tuple
        if (! (bits & ScanOp::SCAN_LCP && thbits & Tuple_header::LCP_SKIP)) {
          Local_key& key_mm = pos.m_key_mm;
          if (! (bits & ScanOp::SCAN_DD)) {
            key_mm = pos.m_key;
            // real page id is already set
          } else {
	    key_mm.assref(th->m_base_record_ref);
            // recompute for each disk tuple
            pos.m_realpid_mm = getRealpid(fragPtr.p, key_mm.m_page_no);
          }
          // TUPKEYREQ handles savepoint stuff
          scan.m_state = ScanOp::Current;
          return true;
        } else {
          jam();
          // clear it so that it will show up in next LCP
          th->m_header_bits = thbits & ~(Uint32)Tuple_header::LCP_SKIP;
	  if (tablePtr.p->m_bits & Tablerec::TR_Checksum) {
	    jam();
	    setChecksum(th, tablePtr.p);
	  }
        }
      }
      break;
  found_deleted_rowid:
      jam();
      {
	ndbassert(bits & ScanOp::SCAN_NR);
	Local_key& key_mm = pos.m_key_mm;
	if (! (bits & ScanOp::SCAN_DD)) {
	  key_mm = pos.m_key;
	  // caller has already set pos.m_get to next tuple
	  // real page id is already set
	} else {
	  key_mm.assref(th->m_base_record_ref);
	  // recompute for each disk tuple
	  pos.m_realpid_mm = getRealpid(fragPtr.p, key_mm.m_page_no);
	  
	  Fix_page *mmpage = (Fix_page*)c_page_pool.getPtr(pos.m_realpid_mm);
	  th = (Tuple_header*)(mmpage->m_data + key_mm.m_page_idx);
	  if ((foundGCI = *th->get_mm_gci(tablePtr.p)) > scanGCI ||
              foundGCI == 0)
	  {
	    if (! (thbits & Tuple_header::FREE))
	      break;
	  }
	}
	
	NextScanConf* const conf = (NextScanConf*)signal->getDataPtrSend();
	conf->scanPtr = scan.m_userPtr;
	conf->accOperationPtr = RNIL;
	conf->fragId = frag.fragmentId;
	conf->localKey[0] = pos.m_key_mm.m_page_no;
	conf->localKey[1] = pos.m_key_mm.m_page_idx;
	conf->gci = foundGCI;
	Uint32 blockNo = refToMain(scan.m_userRef);
	EXECUTE_DIRECT(blockNo, GSN_NEXT_SCANCONF, signal, 6);
	jamEntry();

	// TUPKEYREQ handles savepoint stuff
	loop_count = 32;
	scan.m_state = ScanOp::Next;
	return false;
      }
      break; // incr loop count
    default:
      ndbrequire(false);
      break;
    }
    if (++loop_count >= 32)
      break;
  }
  // TODO: at drop table we have to flush and terminate these
  jam();
  signal->theData[0] = ZTUP_SCAN;
  signal->theData[1] = scanPtr.i;
  sendSignal(reference(), GSN_CONTINUEB, signal, 2, JBB);
  return false;
}

void
Dbtup::handle_lcp_keep(Signal* signal,
                       Fragrecord* fragPtrP,
                       ScanOp* scanPtrP)
{
  TablerecPtr tablePtr;
  tablePtr.i = scanPtrP->m_tableId;
  ptrCheckGuard(tablePtr, cnoOfTablerec, tablerec);

  ndbassert(!fragPtrP->m_lcp_keep_list_head.isNull());
  Local_key tmp = fragPtrP->m_lcp_keep_list_head;
  Uint32 * copytuple = get_copy_tuple_raw(&tmp);
  memcpy(&fragPtrP->m_lcp_keep_list_head,
         copytuple+2,
         sizeof(Local_key));

  if (fragPtrP->m_lcp_keep_list_head.isNull())
  {
    jam();
    ndbassert(tmp.m_page_no == fragPtrP->m_lcp_keep_list_tail.m_page_no);
    ndbassert(tmp.m_page_idx == fragPtrP->m_lcp_keep_list_tail.m_page_idx);
    fragPtrP->m_lcp_keep_list_tail.setNull();
  }

  Local_key save = tmp;
  setCopyTuple(tmp.m_page_no, tmp.m_page_idx);
  NextScanConf* const conf = (NextScanConf*)signal->getDataPtrSend();
  conf->scanPtr = scanPtrP->m_userPtr;
  conf->accOperationPtr = (Uint32)-1;
  conf->fragId = fragPtrP->fragmentId;
  conf->localKey[0] = tmp.m_page_no;
  conf->localKey[1] = tmp.m_page_idx;
  conf->gci = 0;
  Uint32 blockNo = refToMain(scanPtrP->m_userRef);
  EXECUTE_DIRECT(blockNo, GSN_NEXT_SCANCONF, signal, 6);

  c_undo_buffer.free_copy_tuple(&save);
}

void
Dbtup::scanCont(Signal* signal, ScanOpPtr scanPtr)
{
  bool immediate = scanNext(signal, scanPtr);
  if (! immediate) {
    jam();
    // time-slicing again
    return;
  }
  scanReply(signal, scanPtr);
}

void
Dbtup::disk_page_tup_scan_callback(Signal* signal, Uint32 scanPtrI, Uint32 page_i)
{
  ScanOpPtr scanPtr;
  c_scanOpPool.getPtr(scanPtr, scanPtrI);
  ScanOp& scan = *scanPtr.p;
  ScanPos& pos = scan.m_scanPos;
  // get cache page
  Ptr<GlobalPage> gptr;
  m_global_page_pool.getPtr(gptr, page_i);
  pos.m_page = (Page*)gptr.p;
  // continue
  scanCont(signal, scanPtr);
}

void
Dbtup::scanClose(Signal* signal, ScanOpPtr scanPtr)
{
  ScanOp& scan = *scanPtr.p;
  ndbrequire(! (scan.m_bits & ScanOp::SCAN_LOCK_WAIT) && scan.m_accLockOp == RNIL);
  // unlock all not unlocked by LQH
  LocalDLFifoList<ScanLock> list(c_scanLockPool, scan.m_accLockOps);
  ScanLockPtr lockPtr;
  while (list.first(lockPtr)) {
    jam();
    AccLockReq* const lockReq = (AccLockReq*)signal->getDataPtrSend();
    lockReq->returnCode = RNIL;
    lockReq->requestInfo = AccLockReq::Abort;
    lockReq->accOpPtr = lockPtr.p->m_accLockOp;
    EXECUTE_DIRECT(DBACC, GSN_ACC_LOCKREQ, signal, AccLockReq::UndoSignalLength);
    jamEntry();
    ndbrequire(lockReq->returnCode == AccLockReq::Success);
    list.release(lockPtr);
  }
  // send conf
  NextScanConf* const conf = (NextScanConf*)signal->getDataPtrSend();
  conf->scanPtr = scanPtr.p->m_userPtr;
  conf->accOperationPtr = RNIL;
  conf->fragId = RNIL;
  unsigned signalLength = 3;
  sendSignal(scanPtr.p->m_userRef, GSN_NEXT_SCANCONF,
      signal, signalLength, JBB);
  releaseScanOp(scanPtr);
}

void
Dbtup::addAccLockOp(ScanOp& scan, Uint32 accLockOp)
{
  LocalDLFifoList<ScanLock> list(c_scanLockPool, scan.m_accLockOps);
  ScanLockPtr lockPtr;
#ifdef VM_TRACE
  list.first(lockPtr);
  while (lockPtr.i != RNIL) {
    ndbrequire(lockPtr.p->m_accLockOp != accLockOp);
    list.next(lockPtr);
  }
#endif
  bool ok = list.seizeLast(lockPtr);
  ndbrequire(ok);
  lockPtr.p->m_accLockOp = accLockOp;
}

void
Dbtup::removeAccLockOp(ScanOp& scan, Uint32 accLockOp)
{
  LocalDLFifoList<ScanLock> list(c_scanLockPool, scan.m_accLockOps);
  ScanLockPtr lockPtr;
  list.first(lockPtr);
  while (lockPtr.i != RNIL) {
    if (lockPtr.p->m_accLockOp == accLockOp) {
      jam();
      break;
    }
    list.next(lockPtr);
  }
  ndbrequire(lockPtr.i != RNIL);
  list.release(lockPtr);
}

void
Dbtup::releaseScanOp(ScanOpPtr& scanPtr)
{
  FragrecordPtr fragPtr;
  fragPtr.i = scanPtr.p->m_fragPtrI;
  ptrCheckGuard(fragPtr, cnoOfFragrec, fragrecord);

  if(scanPtr.p->m_bits & ScanOp::SCAN_LCP)
  {
    jam();
    fragPtr.p->m_lcp_scan_op = RNIL;
    scanPtr.p->m_fragPtrI = RNIL;
  }
  else
  {
    jam();
    LocalDLList<ScanOp> list(c_scanOpPool, fragPtr.p->m_scanList);    
    list.release(scanPtr);
  }
}

void
Dbtup::execLCP_FRAG_ORD(Signal* signal)
{
  jamEntry();
  LcpFragOrd* req= (LcpFragOrd*)signal->getDataPtr();
  
  TablerecPtr tablePtr;
  tablePtr.i = req->tableId;
  ptrCheckGuard(tablePtr, cnoOfTablerec, tablerec);

  FragrecordPtr fragPtr;
  Uint32 fragId = req->fragmentId;
  fragPtr.i = RNIL;
  getFragmentrec(fragPtr, fragId, tablePtr.p);
  ndbrequire(fragPtr.i != RNIL);
  Fragrecord& frag = *fragPtr.p;
  
  ndbrequire(frag.m_lcp_scan_op == RNIL && c_lcp_scan_op != RNIL);
  frag.m_lcp_scan_op = c_lcp_scan_op;
  ScanOpPtr scanPtr;
  c_scanOpPool.getPtr(scanPtr, frag.m_lcp_scan_op);
  ndbrequire(scanPtr.p->m_fragPtrI == RNIL);
  new (scanPtr.p) ScanOp;
  scanPtr.p->m_fragPtrI = fragPtr.i;
  scanPtr.p->m_state = ScanOp::First;

  ndbassert(frag.m_lcp_keep_list_head.isNull());
  ndbassert(frag.m_lcp_keep_list_tail.isNull());
}
