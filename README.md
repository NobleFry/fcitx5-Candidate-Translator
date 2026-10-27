# fcitx5 Candidate Translator

给 fcitx5 各输入法中包含汉字的候选词追加英语或日语翻译。

Linux 插件通过 fcitx5 官方 `OutputFilter` 仅装饰发送给候选窗的文本副本，不修改输入法拥有的候选对象，默认也不会改变实际提交内容。可选的「提交译文」模式会在提交时通过官方 `CommitFilter` 用译文替代中文，见[提交译文](#提交译文)；未开启时行为与之前完全一致。

1–4 个汉字的短词会优先查询随包安装的 MJdic 离线词典，命中后立即显示；日语读音由本地 Kakasi 生成。词典和持久缓存都未命中时，才调用远程 API。

![](./Screenshot.png)

当前版本支持 Linux 上的 fcitx5 5.1.x，以及 macOS 13.3+ 上的 [fcitx5-macos](https://github.com/fcitx/fcitx5-macos)。插件不限制输入法类型，适用于通过 fcitx5 提供候选列表的所有输入法，并使用 OpenAI Chat Completions 兼容接口；它不支持 macOS 自带的拼音输入法。仅翻译包含汉字且不超过 32 个字符的非占位候选词。候选翻译会发送到你配置的远程服务；密码和敏感输入框不会发送请求。

## Linux 构建与安装

需要 Rust、C++20 编译器、`pkg-config`、fcitx5 开发文件以及网络请求所需的 TLS 根证书。

```bash
cargo test --locked
make build
sudo make install
fcitx5 -r
```

Arch Linux 也可以从当前工作区构建本地包：

```bash
cd packaging
makepkg -si
```

卸载手工安装的文件：

```bash
sudo make uninstall
fcitx5 -r
```

## macOS 构建与安装

macOS 版本需要 macOS 13.3+、Rust、Xcode Command Line Tools，以及已经安装的 fcitx5-macos。下面的命令会优先使用 `/Library/Input Methods/Fcitx5.app/Contents` 中的开发文件；如果安装包不包含开发文件，会自动把官方对应架构的开发包下载到 `target/fcitx5-macos-dev`：

```bash
make test-macos
make package-macos
```

Apple Silicon 会生成：

```text
target/macos-package/candidate-translator-arm64.tar.bz2
```

打开 Fcitx5 的“插件管理器 / Plugin Manager”，选择“手动安装 / Install manually”并选择上述文件。插件管理器会将插件安装到 `~/Library/fcitx5` 并重启 Fcitx5。Intel Mac 的包名为 `candidate-translator-x86_64.tar.bz2`。

可以显式指定目标架构、SDK 或 fcitx5-macos 发布版本：

```bash
MACOS_ARCH=x86_64 make package-macos
FCITX5_MACOS_ROOT='/path/to/Fcitx5.app/Contents' make build-macos
FCITX5_MACOS_RELEASE=0.3.4 make package-macos
```

交叉构建前需要安装对应的 Rust target，例如 `rustup target add x86_64-apple-darwin`。

## 配置

Linux 上打开 `fcitx5-configtool`，返回主界面的“附加组件 / Addons”，搜索“候选词翻译 / Candidate Translator”并点击设置按钮。macOS 上在 Fcitx5 设置中打开同名附加组件。翻译配置属于独立附加组件，不会出现在各输入法自身的配置页中。

填写：

- `Base URL`：兼容接口的 `/v1` 地址，例如 `https://api.example.com/v1`。插件会追加 `/chat/completions`。
- `Model`：服务支持的模型名。
- `API Key`：Bearer Token。
- `Target language`：English 或 Japanese。
- `Show kana reading for Japanese translations`：日语翻译后追加平假名读音，默认开启，例如 `単語を暗記する（たんごをあんきする）`。
- `Reasoning effort`：GPT-5.6 Luna 建议选择 `None`，减少候选翻译延迟；其他模型不支持该参数时选择 `Auto`。
- `Commit mode`：`Never`（默认，仅显示译文）、`When toggled on`（按快捷键切换后，接受候选时上屏的是译文）、`Always`（候选只要已有译文就上屏译文）。
- `Toggle translation commit`：`When toggled on` 模式下的切换快捷键，默认 `Control+Alt+J`。
- `Translate the text before the cursor`：整句翻译快捷键，默认 `Control+Alt+Enter`，见下文。
- `Maximum characters per sentence`：整句翻译最多取多少个字符，默认 120。

三个接口字段默认留空，未填写完整前不会发送请求。API Key 按需求明文保存在 `~/.config/fcitx5/conf/candidate-translator.conf`；插件会尽力将该文件权限收紧为 `0600`，但仍不建议在共享账户中使用长期高权限密钥。

翻译缓存保存在 `~/.cache/fcitx5/candidate-translator/cache-v1.json`，不包含 API Key。配置页勾选 `Clear translation cache on Apply` 后应用，可以清空缓存。

## 提交译文

`Commit mode` 让插件把「输入中文、接受候选」变成「上屏英文」：

- 只替换已经有译文的候选。译文来自持久缓存或随包 MJdic 短词词典，尚未拿到译文时仍然上屏中文，输入不会被阻塞。
- 开启上屏的输入框里，可上屏的候选前面会多一个 `→` 标记，译文改为下划线样式，便于区分「只是提示」和「真的会替换」。
- 切换状态按输入框独立保存，输入框关闭即清除；密码和敏感输入框既不会上屏译文，也不会响应切换快捷键。
- 机制上不拦截接受候选的那次按键：fcitx5 的 `InputContext::commitString` 会把每个提交串送进 `Instance::CommitFilter`，插件用该输入框的「候选原文 → 译文」表做精确匹配，命中才替换。因此标点、英文、表情等非候选提交，以及译文与原文相同的情况都不会被改动。
- 该模式依赖 fcitx5 5.1.20+，与具体输入法无关，fcitx5-pinyin、fcitx5-rime 等都能用；候选窗显示仍是候选级延迟，上屏只使用已经缓存的译文，不会为了上屏而同步请求接口。

## 整句翻译

候选级翻译只覆盖候选词，长句要逐词输入。整句翻译补上这一段：先把中文打完整，再按 `Control+Alt+Enter`，插件取光标前的整段文字翻译，并用译文替换它。

- 取文本：优先用客户端的 surrounding text，从光标往前走，最多 `Maximum characters per sentence` 个字符；遇到换行停止；允许汉字、中文标点、全角符号、英文字母、数字和常见半角标点，开头空格会被丢掉。
- 替换：拿到译文后调用 `InputContext::deleteSurroundingText` 删掉原文，再 `commitString` 上屏译文，所以在支持 surrounding text 的程序里是「原地翻译」。
- 不支持 surrounding text 的客户端（部分 Wayland 应用）：退化为「累积已上屏的中文」，此时只能把译文追加在光标处，中文原文需要自己删掉。
- 结果走同一个缓存；命中缓存时立即替换，不发请求。
- 整句请求不防抖，一次按键一次请求；prompt 要求只返回译文本身，并带 `max_tokens` 上限，避免长篇解释。
- 密码和敏感输入框不响应该快捷键。

### 延迟优化

- 请求复用同一个 HTTP 客户端，省掉每次请求的 TLS 握手（通常 200–600ms）。
- 缓存命中不发请求：整句重复出现时是即时的。
- 缓存键包含 Base URL、模型、推理档位、目标语言和原文，换模型或换档位会各存一份。
- DeepSeek 这类默认开启思考的模型，单次延迟主要来自推理本身：把 `Reasoning effort` 设为 `None`（或服务支持的最低档）能明显变快；整句请求本身已经限制了输出长度。
- `Request timeout` 默认 3 秒，整句翻译在慢模型上可能超时，可以调到 5–15 秒。

## 接口约定

插件批量发送当前页中包含汉字的候选词，并优先通过 Chat Completions 的严格 `response_format: json_schema` 约束响应。prompt 只描述翻译任务。英语 Schema 包含 `index` 和 `text`；日语启用假名注音时还要求平假名 `reading`：

```json
{
  "translations": [
    {"index": 0, "text": "単語を暗記する", "reading": "たんごをあんきする"},
    {"index": 1, "text": "シーツ", "reading": "しーつ"}
  ]
}
```

如果第三方兼容服务明确拒绝 `response_format` 或 `json_schema`，插件会自动用旧式 JSON prompt 重试一次，并在当前 fcitx 进程中记住该 Base URL 与模型不支持结构化输出。请求异步执行并带有防抖；超时、HTTP 错误或无效响应只会暂时保留原候选，不会阻塞输入。

## 开发检查

Rust 与 C++ 的内部接口集中在 `src/bridge.rs`，由 `cxx` 生成绑定。C++ 负责 fcitx5 配置、候选显示和主线程事件；每个插件实例通过 `rust::Box<Translator>` 独立持有 Rust 后端，后端负责词典、缓存、防抖和网络请求。

候选文本先以字节切片传入 Rust，由 Rust 校验 UTF-8、判断是否包含汉字，并限制为最多 32 个 Unicode 标量值。校验通过后才生成跨语言候选字符串；非法或不符合条件的文本保留原候选，不发送翻译请求。密码框、敏感输入框和占位候选由 C++ 根据 fcitx5 状态排除。

后台线程把翻译结果写入队列，通过非阻塞 Unix socket pair 通知 fcitx5 事件循环；Linux 和 macOS 共用这条路径。C++ 在主线程取走结果并检查请求是否仍然有效，后台线程不持有插件或输入上下文指针。配置变更和清空缓存会使旧任务失效，旧响应不会重新填充缓存。卸载时先移除事件监听，再停止并回收后台线程；已经开始的阻塞 HTTP 请求（包括可能的格式回退重试）需要等待返回或超时。

测试需要允许 Unix socket 通信和本机回环 HTTP 服务，覆盖跨语言结果传递、事件通知、实例隔离、请求失效和线程退出。

```bash
cargo fmt --all -- --check
cargo test --locked
cargo build --release --locked
nm -D target/release/libfcitx5_candidate_translator.so | grep fcitx_addon_factory_instance
```

macOS 检查：

```bash
make test-macos
make package-macos
tar tjf target/macos-package/candidate-translator-$(uname -m).tar.bz2
```
