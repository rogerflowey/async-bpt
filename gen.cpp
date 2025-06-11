#include <iostream>
#include <random>
#include <chrono>
#include <numeric>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
template <class T> using Arr = std::vector<T>;

char buf[1000];
std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());

constexpr size_t MAX = 100000;


int Rand() {
	int r = rng();
	return r < 0 ? -r : r;
}
int Rand(int l, int r) { return Rand() % (r - l + 1) + l; }

int main() {
	freopen("test.in","w",stdout);
	int M = 100000;
	Arr<std::string> names(M);
	for (int i = 0; i < M; ++i)
		for (int j = 0; j < 64; ++j)
			names[i] += 'a' + Rand(0, 25);

	Arr<std::string> qry;
#define Q(x, ...) ({ sprintf(buf, x, ##__VA_ARGS__); qry.push_back(buf); })
	Arr<int> perm(M);
	std::iota(perm.begin(), perm.end(), 0);
	std::shuffle(perm.begin(), perm.end(), rng);
	FILE *f = fopen("test.ans", "w");

	Arr<Arr<int>> ans(M);
	int tot = 2000;
	for (int i = 0; i < MAX; ++i) {
		int p = Rand(0, M - 1);
		Q("insert %s %d", names[p].c_str(), ++tot);
		ans[p].push_back(tot);
	}

	for (int i = 0; i < MAX; ++i)
		if (Rand(0, 2) < 2) {
			int p = Rand(0, M - 1);
			Q("insert %s %d", names[p].c_str(), ++tot);
			ans[p].push_back(tot);
		} else {
			int p = Rand(0, M - 1);
			int ra = ans[p].size();
			if (ra > 0) {
				int t = Rand(0, ra - 1);
				Q("delete %s %d", names[p].c_str(), ans[p][t]);
				ans[p].erase(ans[p].begin() + t);
			}
			else
				--i;
		}

	for (int i = 0; i < MAX; ++i) {
		int p = Rand(0, M - 1);
		Q("find %s", names[p].c_str());
		if (ans[p].empty())
			fputs("null\n", f);
		else {
			for (auto x : ans[p])
				fprintf(f, "%d ", x);
			fputs("\n", f);
		}
	}
	printf("%lu\n", qry.size());
	for (auto s : qry)
		printf("%s\n", s.c_str());
	fclose(f);
	return 0;
}
