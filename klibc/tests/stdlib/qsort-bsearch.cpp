#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <vector>

#include "../../src/stdlib/bsearch.cpp"
#include "../../src/stdlib/qsort.cpp"

int cmp_int(const void *a, const void *b) {
	int arg1 = *static_cast<const int *>(a);
	int arg2 = *static_cast<const int *>(b);

	if (arg1 < arg2) {
		return -1;
	}

	if (arg1 > arg2) {
		return 1;
	}

	return 0;
}

struct WeirdStruct {
	char data[17];
	int key;
};

int cmp_struct(const void *a, const void *b) {
	int key1 = static_cast<const WeirdStruct *>(a)->key;
	int key2 = static_cast<const WeirdStruct *>(b)->key;

	if (key1 < key2) {
		return -1;
	}

	if (key1 > key2) {
		return 1;
	}

	return 0;
}

class SearchSortTest : public ::testing::Test {};

TEST_F(SearchSortTest, QSortBasicFunctionality) {
	std::vector<int> data = {5, 2, 9, 1, 5, 6, 8, 3, 0};
	std::vector<int> expected = data;

	std::sort(expected.begin(), expected.end());
	klibc::qsort(data.data(), data.size(), sizeof(int), cmp_int);

	EXPECT_EQ(data, expected);
}

TEST_F(SearchSortTest, QSortTheIdenticalElementTrap) {
	std::vector<int> data(10000, 42);
	klibc::qsort(data.data(), data.size(), sizeof(int), cmp_int);

	for (int val: data) {
		EXPECT_EQ(val, 42);
	}
}

TEST_F(SearchSortTest, QSortLargeRandomizedAndHeapsortFallback) {
	std::vector<int> data(50000);
	std::mt19937 gen(1337);
	std::uniform_int_distribution<> dist(0, 100000);

	for (int &val: data) {
		val = dist(gen);
	}

	std::vector<int> expected = data;
	std::sort(expected.begin(), expected.end());

	klibc::qsort(data.data(), data.size(), sizeof(int), cmp_int);

	EXPECT_EQ(data, expected);
}

TEST_F(SearchSortTest, QSortWeirdStructSizes) {
	std::vector<WeirdStruct> data = {{"a", 99}, {"b", 1}, {"c", 50}, {"d", 42}};

	klibc::qsort(data.data(), data.size(), sizeof(WeirdStruct), cmp_struct);

	EXPECT_EQ(data[0].key, 1);
	EXPECT_EQ(data[1].key, 42);
	EXPECT_EQ(data[2].key, 50);
	EXPECT_EQ(data[3].key, 99);
}

TEST_F(SearchSortTest, BsearchBasicFunctionality) {
	std::vector<int> sorted_data = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90};

	int key = 30;
	void *result = klibc::bsearch(&key, sorted_data.data(), sorted_data.size(), sizeof(int), cmp_int);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(*static_cast<int *>(result), 30);

	key = 35;
	result = klibc::bsearch(&key, sorted_data.data(), sorted_data.size(), sizeof(int), cmp_int);
	EXPECT_EQ(result, nullptr);
}

TEST_F(SearchSortTest, BsearchExponentialBoundsGuarantees) {
	std::vector<int> sorted_data(1000);
	for (int i = 0; i < 1000; ++i) {
		sorted_data[i] = i * 2;
	}

	int first_key = 0;
	void *res1 = klibc::bsearch(&first_key, sorted_data.data(), sorted_data.size(), sizeof(int), cmp_int);
	ASSERT_NE(res1, nullptr);
	EXPECT_EQ(*static_cast<int *>(res1), 0);

	int last_key = 1998;
	void *res2 = klibc::bsearch(&last_key, sorted_data.data(), sorted_data.size(), sizeof(int), cmp_int);
	ASSERT_NE(res2, nullptr);
	EXPECT_EQ(*static_cast<int *>(res2), 1998);

	int missing_beyond = 3000;
	void *res3 = klibc::bsearch(&missing_beyond, sorted_data.data(), sorted_data.size(), sizeof(int), cmp_int);
	EXPECT_EQ(res3, nullptr);
}
