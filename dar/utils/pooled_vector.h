#pragma once

#include "async/async.h"
#include "utils/defines.h"

namespace Dar {

using PooledIndex = SizeType;
#define INVALID_POOLED_INDEX SizeType(-1)

template <class T>
class PooledVector {
public:
	PooledVector() = default;

	PooledIndex push(const T &v) {
		auto lock = mutex.lock();

		if (freeIndices.empty()) {
			arr.push_back(v);
			return arr.size() - 1;
		}

		auto idx = freeIndices.front();
		freeIndices.pop();
		
		arr[idx] = v;
		
		return idx;
	}

	bool release(PooledIndex &index) {
		auto idx = index;
		index = INVALID_POOLED_INDEX;

		if (idx == INVALID_POOLED_INDEX) {
			return false;
		}

		auto lock = mutex.lock();

		if (idx >= arr.size()) {
			return false;
		}

		if (!arr[idx].has_value()) {
			return false;
		}

		arr[idx] = nullOpt;
		freeIndices.push(idx);
		return true;
	}

	const Optional<T>& at(PooledIndex idx) const {
		auto lock = mutex.lock();

		if (idx == INVALID_POOLED_INDEX || idx >= arr.size()) {
			return nullOpt;
		}

		return arr[idx];
	}

private:
	Vector<Optional<T>> arr;
	Queue<PooledIndex> freeIndices;
	mutable SpinLock mutex;
	Optional<T> nullOpt;
};

}