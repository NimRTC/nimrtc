# 切片规格修订记录 —— `v3-n4-d6-connectivity`

> 本文件是**修订记录**，不是规格。
> 已签字的规格 `specs/slices/v3-n4-d6-connectivity.md` 保持原样不动 ——
> 它的 `frozenHash` 是签字时的证据锚点，改写它就等于抹掉签字那一刻的事实。
> 这里记录的是：实现与 frozen spec 的偏离、偏离的原因、以及偏离的范围。

## 一、偏离事实

| 项 | frozen spec（签字时） | 实际实现 |
|---|---|---|
| 连通性 profile 文件路径 | `profiles/connectivity-profiles.json` | `profiles/connectivity/connectivity-profiles.json` |

frozen spec 里提到该路径的地方共 4 处：
`§3` 开头、`§3` 的「不是另一类东西」段、验收标准 6、`§不变量` 段的最后一条。

frozenHash：`sha256:cf09ec41a3ed382857b265cfbbb9740bcd9b8c9eaf27a89a720b5889d0cdc95f`
（记录在 `.zelto/slices/v3-n4-d6-connectivity.json` 的 `spec.frozenHash`）

**修订稿的来历**：实现过程中，规格的 working-tree 副本被就地改写成了
`profiles/connectivity/connectivity-profiles.json`（改写稿一直没提交）。
本次提交把 `specs/slices/v3-n4-d6-connectivity.md` **还原成签字时的版本**
（哈希已核对回 `cf09ec41…`），改写稿移到
`specs/slices/amendments/v3-n4-d6-connectivity.amended.md`，
并在文件头标明**不签字、不覆盖 frozen spec**。

这样处理的理由：改写稿描述的是**实现的做法**，不是**签字时约定的做法**。
把两者混在一个文件里，就再也分不清哪句话是人批准的、哪句话是实现顺手定的。

## 二、为什么偏离

`profiles/` 根目录下的 `*.json` 是**引擎 profile 的命名空间**。

`src/modules/assembly/tests/test_profile_schema.cpp` 的
`ProfileSchema.EveryShippedProfileValidatesUnderV12` 会把
**`profiles/` 根目录下的每一个** `*.json` 拿
`profiles/schema/profile-v1.2.json` 校验：

```cpp
std::vector<fs::path> shipped_profile_files() {
    const fs::path profiles_dir = fs::path{NIMRTC_SOURCE_ROOT} / "profiles";
    std::vector<fs::path> out;
    for (const auto& entry : fs::directory_iterator{profiles_dir}) {
        if (!entry.is_regular_file()) continue;      // ← 子目录被跳过
        if (entry.path().extension() != ".json") continue;
        ...
```

本片新增的文件是「profile 名 → 默认值」的**映射表**，不是单个引擎 profile，
按 v1.2 schema 校验会因缺 `name` 而失败。门禁②的实测错误是：

```
missing required property 'name'
```

放进 `profiles/connectivity/` 子目录即在那个命名空间之外 ——
`fs::directory_iterator` 不递归，`entry.is_regular_file()` 对子目录为 false，
该测试只扫根目录。

## 三、偏离的范围

**只影响文件位置，不影响运行时行为。**

引擎侧读的是代码里写死的 `resolve_connectivity_profile()`；
`profiles/connectivity/connectivity-profiles.json` 是**文档性制品**，
它的位置不参与任何运行时路径解析。

## 四、这次偏离暴露了什么

验收标准 6 写的是「文件存在且完整」，但**没写死它在哪个目录**，
所以这次偏离没有被验收判据拦住 —— 判据只查了「存在 + 键集一致」。

同时，frozen spec 把路径写进了正文四处（`§3`、验收标准 6、`§不变量`），
说明**规格把「放哪」当成了要约束的东西**，
但验收判据没有把它钉成可判定的形式。

这是 zelto 追溯链上的一处缺口：
**规格的措辞比验收判据更严时，偏离规格但通过验收是可能的。**

具体到本例，这次偏离是**好的偏离**（不偏离就过不了门禁②），
但它之所以需要事后补记录而不是当场被拦下，
是因为验收判据没有把「根目录 vs 子目录」写成可判定的断言。

**留给后续的改进方向**（尚未实现，不在 v1.4 范围内）：
验收判据若引用了规格里的具体路径，就应当把该路径写成断言的一部分
（`assert.ok(fs.existsSync(path.join(ROOT, 'profiles/connectivity/connectivity-profiles.json')))`），
而不是只断言「某个地方有个键集一致的文件」。

## 五、相关提交

| 提交 | 内容 |
|---|---|
| `59b09f02` | `feat(v3-n4-d6-connectivity): slice approved` —— 含 frozen spec、验收测试、实现 |

## 六、修订后的规格全文

见本目录同级的 `v3-n4-d6-connectivity.amended.md`（**不签字，不覆盖 frozen spec**）。
