/*
// $Id$
// Fennel is a library of data storage and processing components.
// Copyright (C) 2005-2005 The Eigenbase Project
// Copyright (C) 2005-2005 Disruptive Tech
// Copyright (C) 2005-2005 LucidEra, Inc.
// Portions Copyright (C) 1999-2005 John V. Sichi
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version approved by The Eigenbase Project.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "fennel/common/CommonPreamble.h"
#include "fennel/segment/RandomAllocationSegmentBaseImpl.h"

FENNEL_BEGIN_CPPFILE("$Id$");

// REVIEW:  There are a lot of optimizations possible for reducing the number
// of arithmetic operations required by various methods.  

// TODO:  unit test for segment growth

// TODO:  integrity verifier

// TODO: maintain the PageId of the first SegAllocNode known to have free
// entries (requires synchronization); this would make allocation constant time
// in the common case; also consider using trylock to prevent segment
// allocation nodes from becoming bottlenecks

RandomAllocationSegmentBase::RandomAllocationSegmentBase(
    SharedSegment delegateSegment)
    : DelegatingSegment(delegateSegment)
{
    permAssert(DelegatingSegment::getAllocationOrder() == LINEAR_ALLOCATION);

    // calculate immutable segment parameters based on page size

    nExtentsPerSegAlloc =
        (getUsablePageSize()-sizeof(SegmentAllocationNode))
        / sizeof(SegmentAllocationNode::ExtentEntry);
}

RandomAllocationSegmentBase::~RandomAllocationSegmentBase()
{
}

void RandomAllocationSegmentBase::format()
{
    // calculate number of SegAllocNodes based on current segment size
    uint nSegAllocPages = inferSegAllocCount();

    // calculate number of extents in all but last SegAllocNode
    ExtentNum nExtents = (nSegAllocPages-1)*nExtentsPerSegAlloc;

    // calculate number of pages in last SegAllocNode
    BlockNum nRemainderPages = DelegatingSegment::getAllocatedSizeInPages();
    nRemainderPages -= (nExtents*nPagesPerExtent+nSegAllocPages);
    
    if (nRemainderPages) {
        // last SegAllocNode is not full; add number of remainder extents,
        // rounding down
        nExtents += nRemainderPages/nPagesPerExtent;
    } else {
        // last SegAllocNode is full
        nExtents += nExtentsPerSegAlloc;
    }

    // always format at least one extent; this is somewhat arbitrary, but helps
    // to avoid spurious problems with tiny segments in test cases
    if (!nExtents) {
        nSegAllocPages = 1;
        nExtents = 1;
    }

    // make sure underlying segment is big enough
    bool bigEnough = DelegatingSegment::ensureAllocatedSize(
        nExtents*nPagesPerExtent + nSegAllocPages);
    permAssert(bigEnough);
    
    // format each SegAllocNode
    
    ExtentNum extentNum = 0;
    SegmentAccessor selfAccessor(getTracingSegment(), pCache);
    SegAllocLock segAllocLock(selfAccessor);
    for (uint iSegAlloc = 0; iSegAlloc < nSegAllocPages; iSegAlloc++) {

        PageId segAllocPageId = getSegAllocPageId(iSegAlloc);
        segAllocLock.lockExclusive(segAllocPageId);

        // REVIEW: have to do setMagicNumber() explicitly since we skipped
        // allocation.  Should figure out a way to indicate allocation of a
        // specific PageId.
        segAllocLock.setMagicNumber();
        SegmentAllocationNode &segAllocNode = segAllocLock.getNodeForWrite();

        uint iNextSegAlloc = iSegAlloc + 1;
        if (iNextSegAlloc < nSegAllocPages) {
            // set up SegAllocNode chain
            segAllocNode.nextSegAllocPageId = getSegAllocPageId(iNextSegAlloc);
        } else {
            // terminate SegAllocNode chain at last node
            segAllocNode.nextSegAllocPageId = NULL_PAGE_ID;
        }
        
        // format extent array
        segAllocNode.nPagesPerExtent = nPagesPerExtent;
        segAllocNode.nExtents = std::min(nExtents,nExtentsPerSegAlloc);
        nExtents -= segAllocNode.nExtents;

        // format each extent node within the page
        formatPageExtents(segAllocNode, extentNum);
    }

    permAssert(!nExtents);
}

void RandomAllocationSegmentBase::markPageEntryUnused(
    PageEntry &pageEntry)
{
    pageEntry.ownerId = UNALLOCATED_PAGE_OWNER_ID;
    pageEntry.successorId = NULL_PAGE_ID;
}

uint RandomAllocationSegmentBase::inferSegAllocCount()
{
    BlockNum nPages = DelegatingSegment::getAllocatedSizeInPages();
    // round up
    return nPages/nPagesPerSegAlloc + 
        (nPages%nPagesPerSegAlloc?1:0);
}

PageId RandomAllocationSegmentBase::allocatePageIdFromSegment(
    PageOwnerId ownerId,
    SharedSegment allocNodeSegment)
{
    ExtentNum extentNum = 0;
    SegmentAccessor segAccessor(allocNodeSegment, pCache);
    SegAllocLock segAllocLock(segAccessor);

    // find a SegAllocNode with free space
    PageId origSegAllocPageId = getFirstSegAllocPageId();
    PageId segAllocPageId = getSegAllocPageIdForWrite(origSegAllocPageId);
    for (uint iSegAlloc = 0; ; ++iSegAlloc) {
        // note that we access the node for write, even though we may not
        // actually update it
        segAllocLock.lockExclusive(segAllocPageId);
        SegmentAllocationNode &segAllocNode = segAllocLock.getNodeForWrite();

        // check each extent
        for (uint i = 0; i < segAllocNode.nExtents; ++i, ++extentNum) {
            SegmentAllocationNode::ExtentEntry &extentEntry =
                segAllocNode.getExtentEntry(i);
            if (extentEntry.nUnallocatedPages) {
                // found one
                extentEntry.nUnallocatedPages--;
                // explicit unlock to minimize contention window and avoid
                // deadlock with deallocatePageId; this is like a Southwest
                // airlines reservation: we've got a flight reserved, not a
                // particular seat on that flight, but we're guaranteed to find
                // a seat when we get on
                segAllocLock.unlock();
                return allocateFromExtent(extentNum,ownerId);
            }
        }

        if (segAllocNode.nextSegAllocPageId != NULL_PAGE_ID) {
            // since there's no space on the current SegAllocNode, indicate
            // that we haven't modified it
            undoSegAllocPageWrite(origSegAllocPageId);

            // try next SegAllocNode
            origSegAllocPageId = segAllocNode.nextSegAllocPageId;
            segAllocPageId = getSegAllocPageIdForWrite(origSegAllocPageId);
            continue;
        }
        
        // We have to extend the underlying segment, so we need to prevent
        // anyone else from trying to do the same thing at the same time.  So
        // hold onto segAllocLock during this process.

        if (segAllocNode.nExtents < nExtentsPerSegAlloc) {
            // Try to allocate a new extent.  The parameters to makePageNum
            // request just enough space to fit one more extent within the
            // current SegAllocNode.
            try {
                if (!DelegatingSegment::ensureAllocatedSize(
                        makePageNum(extentNum,nPagesPerExtent)))
                {
                    // couldn't grow
                    undoSegAllocPageWrite(origSegAllocPageId);
                    return NULL_PAGE_ID;
                }
            } catch (...) {
                undoSegAllocPageWrite(origSegAllocPageId);
                throw;
            }

            segAllocNode.nExtents++;
            SegmentAllocationNode::ExtentEntry &extentEntry =
                segAllocNode.getExtentEntry(segAllocNode.nExtents - 1);
            
            // -2 = -1 for extent allocation node, -1 for page we're
            // about to allocate
            extentEntry.nUnallocatedPages = nPagesPerExtent - 2;
            
            return allocateFromNewExtent(extentNum, ownerId);
        }

        // since there's no space on the current SegAllocNode, indicate
        // that we haven't modified it
        undoSegAllocPageWrite(origSegAllocPageId);

        // Have to allocate a whole new SegAllocNode.  The parameters to
        // makePageNum request enough space to fit the first extent of a new
        // SegAllocNode.
        if (!DelegatingSegment::ensureAllocatedSize(
                makePageNum(extentNum+1,0)))
        {
            // couldn't grow
            return NULL_PAGE_ID;
        }

        SegAllocLock newSegAllocLock(segAccessor);
        origSegAllocPageId = getSegAllocPageId(iSegAlloc+1);
        segAllocPageId = getSegAllocPageIdForWrite(origSegAllocPageId);
        newSegAllocLock.lockExclusive(segAllocPageId);
        newSegAllocLock.setMagicNumber();
        SegmentAllocationNode &newNode = newSegAllocLock.getNodeForWrite();
        newNode.nPagesPerExtent = nPagesPerExtent;
        newNode.nExtents = 0;
        newNode.nextSegAllocPageId = NULL_PAGE_ID;
        segAllocNode.nextSegAllocPageId = origSegAllocPageId;

        // Carry on with the loop.  We'll unlock and relock the node just
        // allocated, but that's not a big deal since allocating a new
        // SegAllocNode is very rare.
    }
}

void RandomAllocationSegmentBase::splitPageId(
    PageId pageId,uint &iSegAlloc,
    ExtentNum &extentNum,BlockNum &iPageInExtent) const
{
    // calculate block number relative to containing SegAllocNode
    BlockNum iPageInSegAlloc = getLinearBlockNum(pageId)%nPagesPerSegAlloc;
    iSegAlloc = getLinearBlockNum(pageId)/nPagesPerSegAlloc;
    if (!iPageInSegAlloc) {
        // this is the SegAllocNode itself!
        extentNum = MAXU;
        iPageInExtent = 0;
    } else {
        // account for the SegAllocNode
        --iPageInSegAlloc;
        extentNum =
            iPageInSegAlloc/nPagesPerExtent + nExtentsPerSegAlloc * iSegAlloc;
        iPageInExtent = iPageInSegAlloc%nPagesPerExtent;
    }
}

void RandomAllocationSegmentBase::deallocatePageRange(
    PageId startPageId,
    PageId endPageId)
{
    permAssert(startPageId == endPageId);
    if (startPageId != NULL_PAGE_ID) {
        deallocatePageId(startPageId);
    } else {
        format();
    }
}

void RandomAllocationSegmentBase::deallocatePageId(PageId pageId)
{
    permAssert(pageId != NULL_PAGE_ID);
    assert(isPageIdAllocated(pageId));

    ExtentNum extentNum;
    BlockNum iPageInExtent;
    uint iSegAlloc;
    splitPageId(pageId,iSegAlloc,extentNum,iPageInExtent);
    permAssert(iPageInExtent);

    // note that we mark the free PageId on the extent page BEFORE
    // increasing the corresponding free page count on the segment page;
    // otherwise someone calling allocatePageId at the same time could fail
    freePageEntry(extentNum, iPageInExtent);
    
    SegmentAccessor selfAccessor(getTracingSegment(), pCache);
    SegAllocLock segAllocLock(selfAccessor);
    PageId segAllocPageId = getSegAllocPageId(iSegAlloc);
    segAllocLock.lockExclusive(segAllocPageId);
    SegmentAllocationNode &segAllocNode = segAllocLock.getNodeForWrite();
    ExtentNum relativeExtentNum = extentNum % nExtentsPerSegAlloc;
    segAllocNode.getExtentEntry(relativeExtentNum).nUnallocatedPages++;
    permAssert(
        segAllocNode.getExtentEntry(relativeExtentNum).nUnallocatedPages
        <= nPagesPerExtent);
}

Segment::AllocationOrder RandomAllocationSegmentBase::getAllocationOrder() const
{
    return RANDOM_ALLOCATION;
}

BlockId RandomAllocationSegmentBase::translatePageId(PageId pageId)
{
    assert(
        const_cast<RandomAllocationSegmentBase *>(this)->isPageIdValid(pageId));

    return DelegatingSegment::translatePageId(pageId);
}

bool RandomAllocationSegmentBase::testPageId(
    PageId pageId,
    bool testAllocation,
    bool thisSegment)
{
    if (!DelegatingSegment::isPageIdAllocated(pageId)) {
        return false;
    }
    
    uint iSegAlloc;
    ExtentNum extentNum;
    BlockNum iPageInExtent;
    splitPageId(pageId,iSegAlloc,extentNum,iPageInExtent);
    if (!iPageInExtent) {
        // header pages are valid but not allocated (from the
        // perspective of the RandomAllocationSegment, not the
        // underlying linear segment)
        if (testAllocation) {
            return false;
        } else {
            return true;
        }
    }
    PageOwnerId ownerId = getPageOwnerId(pageId, thisSegment);
    return (ownerId != UNALLOCATED_PAGE_OWNER_ID);
}

bool RandomAllocationSegmentBase::isPageIdValid(PageId pageId)
{
    return testPageId(pageId,false,true);
}

bool RandomAllocationSegmentBase::isPageIdAllocated(PageId pageId)
{
    return testPageId(pageId,true,true);
}

BlockNum RandomAllocationSegmentBase::getAllocatedSizeInPages()
{
    BlockNum nPagesAllocated = 0;

    PageId origSegAllocPageId = getFirstSegAllocPageId();
    do {
        SharedSegment allocNodeSegment;
        PageId segAllocPageId =
            getSegAllocPageIdForRead(origSegAllocPageId, allocNodeSegment);

        PageId nextSegAllocPageId;
        nPagesAllocated +=
            tallySegAllocNodePages(
                segAllocPageId,
                allocNodeSegment,
                nextSegAllocPageId);

        origSegAllocPageId = nextSegAllocPageId;
    } while (origSegAllocPageId != NULL_PAGE_ID);

    return nPagesAllocated;
}

BlockNum RandomAllocationSegmentBase::tallySegAllocNodePages(
    PageId segAllocPageId,
    SharedSegment allocNodeSegment,
    PageId &nextSegAllocPageId)
{
    BlockNum nPagesAllocated = 0;

    SegmentAccessor segAccessor(allocNodeSegment, pCache);
    SegAllocLock segAllocLock(segAccessor);

    // REVIEW zfong 2/5/07 - Do we really need to exclusively lock this page?
    // Isn't a share lock sufficient?
    segAllocLock.lockExclusive(segAllocPageId);
    SegmentAllocationNode const &node = segAllocLock.getNodeForRead();
    uint i;
    for (i = 0; i < node.nExtents; i++) {
        // assume no partial extents; note that -1 is for extent header
        nPagesAllocated +=
            nPagesPerExtent
            - node.getExtentEntry(i).nUnallocatedPages
            - 1;
    }
    nextSegAllocPageId = node.nextSegAllocPageId;
    return nPagesAllocated;
}

FENNEL_END_CPPFILE("$Id$");

// End RandomAllocationSegmentBase.cpp