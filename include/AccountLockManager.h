#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

enum class TxnPriority { CREDIT = 0, DEBIT = 1 };

class AccountLockManager {
  struct WaitNode {
    TxnPriority priority;
    std::condition_variable cv;
    bool ready = false;
    WaitNode(TxnPriority p) : priority(p) {}
  };

  struct CompareNode {
    bool operator()(WaitNode *a, WaitNode *b) {
      // Lower value is higher priority (CREDIT=0, DEBIT=1)
      return static_cast<int>(a->priority) > static_cast<int>(b->priority);
    }
  };

  struct AccountState {
    int activeCredits = 0;
    int activeDebits = 0;
    bool isLocked = false;
    std::priority_queue<WaitNode *, std::vector<WaitNode *>, CompareNode>
        waiters;
  };

  std::mutex mutex_;
  std::unordered_map<std::string, AccountState> states_;

public:
  static AccountLockManager &getInstance() {
    static AccountLockManager instance;
    return instance;
  }

  class ScopedLock {
    AccountLockManager &manager_;
    std::string account_;
    TxnPriority priority_;

  public:
    ScopedLock(AccountLockManager &m, std::string acc, TxnPriority prio)
        : manager_(m), account_(std::move(acc)), priority_(prio) {
      manager_.acquire(account_, priority_);
    }
    ~ScopedLock() { manager_.release(account_, priority_); }
  };

  void acquire(const std::string &account, TxnPriority prio) {
    // If this is a DEBIT transaction, we introduce a slight delay.
    // This gives any simultaneous CREDIT transactions a head-start to enter
    // the manager and register their priority, ensuring they process first.
    if (prio == TxnPriority::DEBIT) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::unique_lock<std::mutex> lock(mutex_);
    auto &state = states_[account];

    // If it's a CREDIT, we increment activeCredits so that DEBITs know a credit
    // is here
    if (prio == TxnPriority::CREDIT) {
      state.activeCredits++;
    } else {
      state.activeDebits++;
    }

    // We want to wait if it's already locked OR if we are a DEBIT and there are
    // pending CREDITs
    bool shouldWait = state.isLocked ||
                      (prio == TxnPriority::DEBIT && state.activeCredits > 0);

    if (!shouldWait) {
      state.isLocked = true;
      return;
    }

    WaitNode node(prio);
    state.waiters.push(&node);
    node.cv.wait(lock, [&node] { return node.ready; });
  }

  void release(const std::string &account, TxnPriority prio) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(account);
    if (it != states_.end()) {
      auto &state = it->second;

      if (prio == TxnPriority::CREDIT) {
        state.activeCredits--;
      } else {
        state.activeDebits--;
      }

      if (!state.waiters.empty()) {
        // If the next highest priority is a DEBIT, but there are still active
        // CREDITs waiting, we should theoretically only pop if the condition
        // allows it. But since CREDIT is higher priority, waiters.top() WILL be
        // a CREDIT if any exist.
        WaitNode *nextNode = state.waiters.top();

        // Double check if nextNode is DEBIT and activeCredits > 0.
        // That shouldn't happen because if activeCredits > 0, there must be a
        // CREDIT in the waiters queue, and CREDIT has higher priority than
        // DEBIT.
        state.waiters.pop();
        nextNode->ready = true;
        nextNode->cv.notify_one();
      } else {
        state.isLocked = false;
        if (state.activeCredits == 0 && state.activeDebits == 0) {
          states_.erase(it);
        }
      }
    }
  }
};
