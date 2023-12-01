#include <iostream>
#include <vector>
using namespace std;

int findFirstLarger(const vector<int>& nums, int target) {
    int left = 0, right = nums.size() - 1;
    int ans = nums.size(); // 如果没有找到，返回数组长度

    while (left <= right) {
        int middle = left + (right - left) / 2;

        if (nums[middle] > target) {
            ans = middle; // 记录比目标大的元素的位置
            right = middle - 1; // 继续在左侧查找
        } else {
            left = middle + 1; // 在右侧查找
            //cout<<left<<endl;
        }
    }

    return ans;
}

int main() {
    vector<int> nums = {1, 3, 5, 7, 9};
    int target = 4;
    int index = findFirstLarger(nums, target);
    cout << "111The index of the first element larger than " << target << " is " << index << endl;
    return 0;
}

/*
创建测试用例时，应考虑各种可能的场景，以确保算法的正确性和健壮性。针对您提供的C++函数 findFirstLarger，我们可以设计以下测试用例：

常规测试用例：

输入：nums = [1, 3, 5, 7, 9], target = 4
预期输出：2（因为nums[2] = 5是第一个大于4的元素）
目标值小于所有元素：

输入：nums = [10, 20, 30, 40, 50], target = 5
预期输出：0（因为nums[0] = 10是第一个大于5的元素）
目标值大于所有元素：

输入：nums = [1, 2, 3, 4, 5], target = 10
预期输出：5（数组长度，因为没有元素大于10）
空数组：

输入：nums = [], target = 5
预期输出：0（数组长度为0）
单元素数组：

输入：nums = [5], target = 3
预期输出：0（因为nums[0] = 5是第一个大于3的元素）
另一个案例：nums = [2], target = 3
预期输出：1（数组长度，因为没有元素大于3）
含有重复元素：

输入：nums = [1, 2, 2, 2, 3, 4, 5], target = 2
预期输出：5（因为nums[5] = 4是第一个大于2的元素）
目标值正好是数组中的元素：

输入：nums = [1, 2, 3, 4, 5], target = 3


*/
