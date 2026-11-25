# 追溯链缺陷记录 —— frozenHash 不跨平台可比

> 这不是某一片的修订，是**追溯链本身的**一个缺口。
> 记录在这里是因为它第一次在 v1.4 的收尾核对中被撞到。

## 现象

`v1-audio-p2` 这片的 `spec.frozenHash` 与工作树里的 `specs/slices/v1-audio-p2.md`
对不上：

```
记录的 frozenHash     : sha256:6da73a0674fd5c5b8c8a5a50a848cf012ae404116671b126d4d6cb2676b9c623
工作树文件（LF）的哈希 : sha256:914c1fb03ee8bd4f6eb1c2a356784c2cd407e092a97cd5a5352ee0ffa887a062
```

## 根因

**不是内容变了，是行尾变了。** 把 LF 版本转成 CRLF，哈希正好等于记录的 frozenHash：

```
CRLF 版本的哈希 : sha256:6da73a0674fd5c5b8c8a5a50a848cf012ae404116671b126d4d6cb2676b9c623  ← 命中
```

冻结是在 Windows 上做的，当时仓库的 `core.autocrlf=true` 生效，
checkout 出来的工作树文件是 **CRLF**；冻结算的是**工作树文件的字节**。
而 git 存储和提交的是 **LF** 版本（`.gitattributes` 里 `* text=auto` 做规范化）。

于是产生了这个局面：

* **冻结时算的哈希** = CRLF 字节的哈希 —— 这是签字那一刻人看到的东西
* **仓库里存的字节** = LF —— 这是 git 会 checkout 出 LF 之后人看到的东西

## 为什么这是缺陷

frozenHash 的作用是「事后能检出规格被改过」。要做到这件事，
哈希必须**只依赖规格的内容，不依赖它碰巧以什么行尾存着**。

现在这个锚点有两个问题：

1. **在 Windows 上它对不上自己签的规格**。签字时是 CRLF，事后 checkout 出来是 LF，
   重新算就对不上 —— 看起来像「规格被改了」，实际是「行尾被规范化了」。
2. **它对同一条规格在不同机器上的判定不同**。Linux 上从没有 CRLF，
   同样的规格在任何时候都算不出这个哈希。

一个「检出篡改」的锚点，如果它自己会因为平台而误报，那它在误报的时候
就无法被信任 —— 而追溯链最怕的就是这种「说不清是不是真被改了」的时刻。

## 影响范围

**只有 `v1-audio-p2` 一片。** 8 片的实测结果：

| 切片 | frozenHash 匹配的是 |
|---|---|
| `v0-cmake`、`v0-src-log`、`v1-cmake`、`v1-device-policy`、`v1-n5-turn-server`、`v2-ice`、`v3-n4-d6-connectivity` | LF |
| **`v1-audio-p2`** | **CRLF** |

其余 7 片冻结时 `.gitattributes` 已经把 `*.md` 归到 LF 了，所以只有这一片
是早期 Windows checkout 的遗留。

**规格内容本身没有问题** —— `version/v3` 上提交的 `v1-audio-p2.md`
与冻结时的内容逐字相同，只有行尾不同。

## 修法（未实施，仅记录）

**冻结时哈希工作树字节**这个做法本身就该改：哈希应当算在
**规范化之后**（LF）的字节上，这样任何平台算出来的都一样。

具体到 zelto 侧，改动点很小 —— `src/core/lifecycle/freeze.ts` 的
`hashSpecFile()`：

```ts
export function hashSpecFile(specPath: string): string | null {
  try {
    return hashContent(readFileSync(specPath, 'utf-8'));   // ← 原样哈希，行尾进了哈希
  } catch {
    return null;
  }
}
```

读出来是 `utf-8` 字符串，CRLF 在字符串里原样保留，于是**行尾进了哈希**。
改成先把 CRLF 归一成 LF 再哈希即可 —— `.gitattributes` 已经声明了
`* text=auto`，规范化规则是现成的，只是没被哈希这一步用上。

**这一改动属于 zelto 的契约变更**（切片状态文件里哈希的语义变了），
按 AGENTS.md 的规则需要先写 ADR 提案，不在 v1.4 的范围内。

## 眼下怎么办

`v1-audio-p2` 这一片已经签字完成、已经合入 `version/v1`，
它的规格内容没有争议。

**比对 frozenHash 的地方应当先规范化行尾**，涉及三处：
* `src/core/lifecycle/freeze.ts` 的 `hashSpecFile()`（冻结时写锚点）
* `src/cli/commands/core-slice.ts` 的 `zelto check`（校验时比对）
* `src/cli/commands/slice-detail.ts` 的规格哈希显示

其中 `src/cli/commands/shared.ts` 注释里引用的 **ADR-086** 已经点明了
这个坑的形状 ——「同一个东西两份实现、格式悄悄不一样」，
表现为「规格刚冻结完就报 hash mismatch」，而症状离病因很远。
行尾是同一类问题的另一种形式：**同一个东西两种字节表示**。

这不是「放宽校验」—— 规格的内容一个字都没变，只是比对的字节表示
统一到 `.gitattributes` 声明的规范形式。

## 为什么现在才发现

因为它**只在一种组合下才可见**：
「Windows 上冻结」+「`.gitattributes` 后来才把 `*.md` 归到 LF」+
「换机器或重新 checkout」。

`v1-audio-p2` 恰好撞上了这三样 —— 它是 v1.4 里最早冻结的一片，
冻结时 `*.md` 还没被 `.gitattributes` 覆盖。之后 7 片都在 LF 下冻结，
所以看不出问题。

**教训**：这类缺陷不会被「跑一遍看看」发现，只会被「逐片核对锚点」发现。
收尾核对时把 8 片全验一遍，是唯一能撞出它的动作。

