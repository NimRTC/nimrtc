# 切片规格修订记录 —— `v3-n4-d6-connectivity`

> 本文件是**修订记录**，不是规格。
> 已签字的规格 `specs/slices/v3-n4-d6-connectivity.md` 保持原样不动 ——
> 它的 `frozenHash` 是签字时的证据锚点，改写它就等于抹掉签字那一刻的事实。
> 这里记录的是：实现与 frozen spec 的偏离、偏离的原因、以及偏离的范围。

## 一、偏离事实

| 项 | frozen spec（签字时） | 实际实现 |
|---|---|---|
| 连通性 profile 文件路径 | `profiles/connectivity-profiles.json` | `profiles/connectivity/connectivity-profiles.json` |
| `credentials-rejected` 的来源 | 未指明 | **seam**（`IICETransport::turn_failure_reason()`），见修订稿「决定一」 |
| `turn_transport_used` 的来源 | 未指明 | **引擎自己的配置**，见修订稿「决定二」 |
| 默认 STUN 服务器 | 未定义 | `stun.l.google.com:3478`，见修订稿「决定三」 |
| 失败原因的优先级 | `no-pair` 在 `checks-failed` **前** | `checks-failed` 在 `no-pair` **前**，见修订稿「决定四」 |

后四项不是「偏离」——frozen spec 在那些地方**没有说**，实现补上的是
spec 留白的地方。之所以要记，是因为一个没写的 spec 也会被实现**读成**
一个意思，而事后无法从 frozen spec 里分辨「签字时就这么定的」和
「实现顺手定的」。修订稿是那个区分的地方。

frozen spec 里提到 profile 路径的地方共 4 处：


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

**第二次修订的来历**：实现继续推进时，frozen spec 的 working-tree 副本
**又被**就地加进了三个实现期的决定（决定一 / 二 / 三）。
那一版同样**没有提交**，但它一度是工作区里的样子——
如果照那个样子提交，frozen spec 的哈希就变了，签字时的锚点就没了。
本次提交把它**再次还原**回 `cf09ec41…`，
三个决定搬进了修订稿的「本片实现期的三个决定」一节。

这是同一类错误的第二次发生，所以它值得被单独记下来：
**「改写稿在另一个文件里」这件事，只有在 frozen spec 的哈希被核对过
之后才算成立**。实现期每往规格里加一句话，都要问一句：
这句话是「签字时约定的」还是「实现时决定的」？后者属于修订稿。


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

决定一 / 二 / 三 / 四 同样**不改变 frozen spec 的结论**：
决定一是**补上** frozen spec 没说的「凭据被拒从哪来」，
决定二是**补上**「turn_transport_used 从哪读」，
决定三是**补上** frozen spec 说「STUN 被配上」却没配哪个，
决定四是**调换**了 frozen spec 明确写下的两条优先级。

只有决定四改变了 frozen spec 的**字面内容**——它没有改变结论
（两个理由仍然是两种理由），但它改了顺序，而顺序是 frozen spec
**明确写过**的。所以决定四是本记录里唯一一条「真的偏离」，
其余三条是「填空」。这个区分很重要：一个改了顺序的优先级，
如果只在修订稿里体现，审规格的人会从 frozen spec 读到另一个顺序，
然后**照着 frozen spec 写测试**，然后测试失败。
所以决定四必须被显式地指出，而不是混在三条填空里。


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

### 4.1 第二处偏离：写集扩到 v1.4 计划与缺口清单

| 项 | frozen spec（签字时） | 实际实现 |
|---|---|---|
| 本片改动范围 | `src/modules/ice/**`、`src/modules/video_stats/**`、`src/engine/**`、`profiles/**`、验收文件 | 额外含 `docs/plan/v1.4-plan.md` 与 `docs/plan/post-v1.4-gap-inventory.md` |

**为什么必须改。** v1.4 计划漂移探测器
（`tools/check_v14_plan_status.py`）把 N4 与 D6 的断言行写成
「**必须不存在**」——「引擎没有连通性 profile」「stats 没有失败原因词汇」。
条目落地的那一刻，这两行**必须翻转**成「必须存在」，
否则它们会**永远**报 STALE：一个常亮的假警报，只会教会人忽略这个探测器。

而「条目落地时同步计划表与漂移探测器」是本仓库既有的纪律
（见 `docs/plan/v1.4-plan.md` §0a，以及 N5 / D2 落地时的同类翻转）。
所以本片的写集**必须**包含计划文档与探测器脚本，否则计划表会立刻与树不一致。

缺口清单（`post-v1.4-gap-inventory.md`）里那条「探测器当前 N 条断言全 ok」
的数字也要跟着改，否则它会从 20 变成一个错的数。

**偏离的范围**：**只影响「本片能改哪些文件」，不影响任何运行时行为，
也不改变 frozen spec 的任何结论。** frozen spec 的正文一个字没动，
哈希仍是 `cf09ec41…`。

**为什么它值得单独记一笔。** 验收判据（门禁③「本片改动仅限写集范围」）
本来是**比规格更严**的东西：规格没写的文件，按判据就不许改。
所以这一次是**判据与实现相反**——判据先红了，实现才去改判据。
如果只改判据不记这一笔，下一个读者会以为
「写集可以随便扩」；而事实是：**写集只能因为「树的一致性要求」而扩，
不能因为「实现顺手改了别的」而扩**，而这个理由必须落在纸上。


## 五、相关提交

| 提交 | 内容 |
|---|---|
| `59b09f02` | `feat(v3-n4-d6-connectivity): slice approved` —— 含 frozen spec、验收测试、实现 |

## 六、修订后的规格全文

见本目录同级的 `v3-n4-d6-connectivity.amended.md`（**不签字，不覆盖 frozen spec**）。
