Worked for 20s

はい。カメラ入力から前処理・推論・描画まで、デバイスメモリ上で連結することを明確に設計している実装があります。

特に参考になるのは、次の3つです。

NVIDIA DeepStream：GPU完結型パイプラインとして最も完成度が高い
OpenVINO Remote Tensor：WindowsでGPU・NPUメモリ共有を設計する際に重要
MediaPipe GPU Graph：GPU処理と描画を同一グラフ内で接続する設計として優秀

ONNX RuntimeにもDevice TensorとI/O Bindingがありますが、こちらは完成済み映像パイプラインというより、自分のアプリでメモリ管理を組み立てるための部品です。

1. NVIDIA DeepStream
最も「カメラ→GPU→推論→GPU描画」に近い

DeepStreamは、映像をGStreamerのバッファとして流しながら、デコード、色変換、推論、追跡、描画、表示をGPU上で接続することを目的としています。

典型的には次の構成です。

Camera / encoded video
        ↓
Hardware decoder
        ↓
NvBufSurface / NVMM
        ↓
GPU color conversion
        ↓
TensorRT inference
        ↓
GPU tracker
        ↓
GPU overlay
        ↓
EGL / display

映像フレームは通常のmalloc()されたCPUメモリではなく、NvBufSurfaceで管理されるNVMM/CUDA系バッファとしてステージ間を流れます。gst-nvvideoconvertもNVMMを入力・出力にできるため、色変換の前後で必ずCPUへ戻す必要がありません。

トラッカーも入力として、CUDA device memoryまたはCPU-mapped NVMMを要求できる設計です。つまり、トラッカーが画像を必要としない場合はメタデータだけを渡し、必要な場合もGPU側の画像を受け取れます。

メモリ上では何が起きるか

理想的には次のようになります。

Camera DMA
    ↓
NV12 GPU-accessible surface
    ↓
GPU resize / color conversion
    ↓
TensorRT input tensor
    ↓
TensorRT output tensor
    ↓
CPUには座標などの小さいmetadataのみ
    ↓
GPU overlay

重要なのは、画像本体をCPUへコピーしないことです。

たとえば姿勢推定なら、GPUからCPUへ戻す必要があるのは、通常は次のような小さい結果だけです。

struct Landmark {
    float x;
    float y;
    float z;
    float confidence;
};

33点であれば数百バイト程度なので、画像フレーム数MBを毎回転送するのとはコストが全く異なります。

さらに徹底する場合は、ランドマーク結果もGPUに残したまま、CUDA kernelやGPU描画処理から直接読み取れます。

TensorRT output GPU buffer
        ↓
CUDA postprocess
        ↓
GPU vertex buffer
        ↓
OpenGL / EGL rendering
高速化手法

DeepStreamはメモリ共有以外にも次を組み合わせています。

ハードウェアデコード

H.264/H.265などをCPUではなく専用デコーダで処理します。

encoded stream
    ↓ NVDEC
NV12 GPU surface

デコード後の画像が最初からGPU側にあるため、

CPU decode
→ CPU RGB
→ GPU upload

を避けられます。

バッチ処理

複数カメラや複数フレームをまとめてTensorRTへ投入します。

camera 1 ┐
camera 2 ├→ batch tensor → TensorRT
camera 3 ┘

GPU利用率は上がりますが、単一カメラの低遅延アプリでは、無理にバッチサイズを増やすと待ち時間が増えることがあります。

推論間引きと追跡
frame 0: detection + pose
frame 1: tracking
frame 2: tracking
frame 3: detection + pose

推論を毎フレーム行わず、間のフレームをトラッカーで補います。

TensorRT最適化
FP16
INT8
レイヤー融合
最適カーネル選択
固定shape
Tensor memory reuse

などを利用します。

評価

DeepStreamは、設計を読む対象としては最良です。

ただし、NVIDIA GPUまたはJetsonを前提とするため、Windows上でIntel GPU、AMD GPU、Qualcomm NPUなども含めて動かしたい場合、そのまま採用するものではありません。

2. OpenVINO Remote Tensor
WindowsでGPU・NPUメモリ共有を考えるなら重要

OpenVINOにはov::RemoteContextとov::RemoteTensorがあります。

通常の推論では次のコピーが発生します。

D3D texture
    ↓ GPU → CPU
CPU image
    ↓ CPU preprocessing
CPU tensor
    ↓ CPU → GPU/NPU
Inference

Remote Tensorを利用すると、既存のネイティブAPIが所有するメモリを、OpenVINOのTensorとしてラップできます。

GPUプラグインは、WindowsでMicrosoft DirectXとの相互運用をサポートし、既存GPUパイプラインへ推論を接続するときのコピーを避けることを目的としています。

概念的には次の構造です。

Media Foundation camera
        ↓
ID3D11Texture2D
        ↓
DirectX / OpenCL shared resource
        ↓
ov::RemoteTensor
        ↓
OpenVINO GPU inference
GPUの場合

OpenVINO GPUプラグインは内部的にはOpenCLコンテキストを使います。

Windowsでは、DirectXリソースとの共有用APIが用意されています。

#include <openvino/runtime/intel_gpu/ocl/dx.hpp>

OpenVINO GPUプラグインは、DirectX、OpenCL、VAAPIなどのネイティブメモリをRemote Tensorとして扱い、GPUパイプラインへの組込み時にコピーを避けることを目的としています。

構成は次のようになります。

D3D11 camera texture
        ↓ shared handle
OpenCL/OpenVINO remote image
        ↓
GPU resize / color conversion
        ↓
GPU inference

ただし、単にID3D11Texture2D*をset_tensor()へ渡せばよいわけではありません。

次の条件を合わせる必要があります。

GPUアダプターが同じ
DirectXリソースが共有可能
テクスチャフォーマットが対応している
OpenCL/DirectX interopが利用可能
リソース同期が正しい
推論中にカメラ側がテクスチャを書き換えない

したがって、ゼロコピーは単なるAPI呼び出しではなく、所有権と同期を含む設計問題です。

NPUの場合

OpenVINOのNPUプラグインにもRemote Tensor APIがあります。

OpenCL、Vulkan、DirectX 12などとのメモリ共有をサポートし、WindowsではNT handleを利用します。NPU側ではLevel Zeroコンテキストとメモリオブジェクトを共有します。

概念的には次の構造です。

D3D12/shared memory
        ↓ NT handle
OpenVINO NPU RemoteTensor
        ↓
NPU inference

ただし、ここで注意が必要です。

NPUで推論するとき、カメラ映像の描画は通常GPUです。

GPU: camera capture / render
NPU: neural inference
CPU: orchestration

したがって、完全に一つのデバイス上で完結するわけではありません。

Camera texture on GPU
        ↓
shared memory / interop
NPU input
        ↓
small inference result
        ↓
CPU or GPU
        ↓
GPU drawing

NPU利用の狙いは、GPUコピーを完全になくすことよりも、

GPUを描画専用に空ける
消費電力を抑える
長時間バックグラウンド推論を行う
GPU負荷によるUIのフレーム落ちを避ける

ことにあります。

評価

対象PCがIntel Core Ultraなどで、OpenVINO NPUを利用するなら非常に参考になります。

また、Windowsネイティブアプリで、

Media Foundation
Direct3D 11
OpenVINO GPU/NPU
Direct2D

を接続する設計にも適しています。

3. MediaPipe GPU Graph

MediaPipeも、GPUメモリ上の画像を複数Calculator間で渡し、GPU推論とGPU描画を同じグラフに入れる思想を持っています。

公式ドキュメントでも、

GPU Calculatorをグラフの任意位置に置ける
GPU間のフレーム転送で高価なコピーを起こさない
GPUを推論だけでなく描画にも使う
CPU/GPU混成グラフを構築する

ことが設計原則として明示されています。

典型的な構成です。

Camera texture
      ├──────────────→ GPU renderer
      │
      ↓
GPU preprocessing
      ↓
GPU inference
      ↓
CPU landmarks
      ↓
GPU renderer

あるいは、後処理もGPUに置けば、

Camera texture
      ↓
GPU preprocessing
      ↓
GPU inference
      ↓
GPU postprocess
      ↓
GPU rendering

とできます。

Android/LinuxではOpenGL ES 3.1以上を使ってGPU推論を行えます。MediaPipeはLinuxデスクトップでもGPU compute、GPU rendering、TFLite GPU inferenceを実行できます。

特に参考になる点

MediaPipeでは、GPUリソースを単にvoid*として流すのではなく、GpuBufferのような抽象型としてグラフ内を流します。

GpuBuffer
   ↓
GlCalculator
   ↓
GpuBuffer

これによって、

どのCalculatorが入力を読むか
どこでCPUへ変換するか
GPUコンテキストをどう共有するか
描画と推論の依存関係

をグラフとして表現できます。

制約

MediaPipeはモバイルとLinux上のOpenGL ESを中心に設計されています。

Windowsネイティブの

Media Foundation
D3D11
Direct2D
ONNX Runtime

へそのまま適用するのは難しく、主に設計思想の参考です。

4. ONNX Runtime Device TensorとI/O Binding

ONNX Runtimeには、ユーザーがCPU/GPUメモリの配置を明示的に管理する仕組みがあります。

通常は次のようになります。

session.Run(
    cpuInput,
    cpuOutput
);

この場合、Execution Providerが内部で、

CPU input → GPU input
GPU inference
GPU output → CPU output

を実行することがあります。

I/O Bindingを使うと、

GPU input tensor
    ↓
ONNX Runtime
    ↓
GPU output tensor

として、入力・出力の配置を指定できます。

ONNX RuntimeのDevice Tensorは、デコード済み映像がすでにGPU上にあるケースや、出力を後続のGPU処理へ渡すケースを明示的な用途として挙げています。固定アドレスを利用することで、CUDA Graphや非同期実行にもつなげられます。

CUDAの場合
Ort::MemoryInfo memoryInfo(
    "Cuda",
    OrtAllocatorType::OrtDeviceAllocator,
    deviceId,
    OrtMemTypeDefault
);

Ort::Value inputTensor =
    Ort::Value::CreateTensor(
        memoryInfo,
        cudaDevicePointer,
        bufferSize,
        shape.data(),
        shape.size(),
        elementType
    );

これをI/O Bindingへ渡します。

ioBinding.BindInput("input", inputTensor);
ioBinding.BindOutput("output", outputTensor);

session.Run(runOptions, ioBinding);

さらにCUDA streamを推論側とアプリ側で共有できます。

CUDA preprocessing stream
        ↓ same stream
ONNX Runtime CUDA inference
        ↓ same stream
CUDA postprocessing

これにより不要なcudaDeviceSynchronize()を減らせます。

DirectMLの場合

DirectML Execution Providerは、既存のIDMLDeviceとD3D12 command queueを渡して構築できます。つまり、自分のD3D12描画・計算パイプラインと同一デバイス・同一キュー系統へ推論を統合できます。

ただし注意点があります。

DirectML EPでは同じSessionに対する並列Run()は不可
memory pattern optimizationを無効化する必要がある
固定shapeの方が高速
D3D11カメラテクスチャをD3D12推論リソースへ接続するにはinteropが必要

DirectMLはWindowsで広範なGPUに対応しますが、映像パイプラインの実装自由度は高い一方、DeepStreamほど一体化された完成品ではありません。

評価

あなたが既にONNX Runtimeを使う前提であれば、最も現実的な段階的改善方法です。

最初は、

CPU camera frame
→ CPU preprocessing
→ ONNX Runtime

で動かし、その後、

D3D texture
→ GPU preprocessing
→ Device Tensor / I/O Binding
→ GPU inference

へ移行できます。

メモリコピー以外の高速化

ゼロコピーだけを実装しても、必ずしも最大の高速化にはなりません。

リアルタイム姿勢推定では、次の最適化の方が効く場合があります。

1. DetectorとLandmarkモデルを分ける

MediaPipe PoseやHand系で使われる方式です。

初回:
full-frame detector
    ↓
ROI

次フレーム:
previous ROI
    ↓
landmark model
    ↓
new ROI

毎回フル画像から検出するのではなく、前フレームの結果からROIを更新します。

毎フレーム検出:
1920 × 1080 → detector

ROI tracking:
256 × 256 → landmark model

この差は、メモリコピー最適化より大きいことがあります。

2. 最新フレーム優先

推論キューをFIFOにしないことです。

悪い構成です。

capture:
100, 101, 102, 103, 104

inference:
100 → 101 → 102 → ...

良い構成です。

capture latest:
104

inference:
104

フレームを捨てても、表示時点に近いフレームを推論します。

これはFPSそのものを上げる最適化ではなく、motion-to-photon latencyを下げる最適化です。

3. 推論FPSと描画FPSを分離
Capture:   30 FPS
Inference: 15 FPS
Render:    60 FPS

推論結果が更新されないフレームでは、直前の結果を描画します。

必要ならランドマークを補間します。

displayPoint =
    previousPoint * (1.0f - alpha)
    + currentPoint * alpha;
4. 前処理をGPUへ移す

推論本体がGPUでも、以下をCPUで行っているとボトルネックになります。

NV12 → RGB
resize
crop
normalize
HWC → CHW
uint8 → float16/float32

理想的には、

camera NV12 texture
    ↓ GPU shader
resize + crop + normalize
    ↓
model input tensor

とします。

カスタムcompute shaderやCUDA kernelを使う方法と、モデル内へ前処理を統合する方法があります。

5. モデル出力後処理をGPUへ移す

物体検出では特に重要です。

GPU model output
    ↓
CPU readback
    ↓
CPU NMS

ではなく、

GPU model output
    ↓
GPU decode
    ↓
GPU NMS
    ↓
small final detections only readback

とします。

姿勢推定では出力が比較的小さいため優先度は下がりますが、ヒートマップ出力モデルでは有効です。

6. 固定shape

入力shapeを固定します。

dynamic:
[1, 3, H, W]

fixed:
[1, 3, 256, 256]

DirectMLでは固定shapeにより、定数畳み込み、事前初期化、重みの前処理、より適切なtensor layoutなどの最適化が可能になります。

7. FP16・INT8量子化
FP32
↓
FP16
↓
INT8

通常は演算量とメモリ帯域の両方を削減できます。

ただし、

keypoint精度低下
小さい物体への影響
キャリブレーションデータ不足
CPU/NPU/GPUごとの量子化対応差

を確認する必要があります。

姿勢推定では、まずFP16を試し、その後INT8を評価する方が安全です。

8. バッファ再利用

毎フレーム次を行うべきではありません。

std::vector<float> input(inputSize);
auto tensor = CreateTensor(...);

代わりに、初期化時に確保します。

class InferenceBuffers {
public:
    DeviceBuffer input;
    DeviceBuffer output;
    DeviceBuffer scratch;
};

毎フレームは同じメモリを再利用します。

これにより、

heap allocation
GPU allocation
resource creation
page fault
descriptor作成

を減らせます。

9. Double/Triple buffering

カメラ、推論、描画が同じテクスチャを奪い合わないようにします。

Texture A: camera writing
Texture B: inference reading
Texture C: rendering

次のフレームで役割を回します。

A → B
B → C
C → A

ただし、単に3枚用意するだけでは不十分で、fenceやkeyed mutexで利用完了を確認する必要があります。

10. 非同期パイプライン
frame N:
GPU preprocess

frame N-1:
GPU/NPU inference

frame N-2:
GPU render

のように重ねます。

time ─────────────────────────────>

preprocess: [N]    [N+1]  [N+2]
inference:      [N]    [N+1]
render:              [N]    [N+1]

同一GPU上では処理が完全並列になるとは限りませんが、CPU待機やコピー待機を隠せます。

あなたのWindowsアプリに対する推奨構成

現在の前提が、

Windows
Media Foundation
Direct3D 11
Direct2D
ONNX Runtime
GPUまたはNPU推論

であれば、次の構成が現実的です。

GPU推論の場合
Media Foundation Source Reader
        ↓
IMFDXGIBuffer
        ↓
ID3D11Texture2D
        ├──────────────────────────────┐
        │                              │
        ↓                              ↓
D3D11 compute shader             D2D camera rendering
resize / crop / normalize
        ↓
shared inference resource
        ↓
ONNX Runtime
DirectML / OpenVINO GPU
        ↓
small landmark output
        ↓
D2D overlay rendering
        ↓
DXGI flip-model swap chain

ここで狙うべきなのは、画像本体を一度もCPUへ戻さないことです。

CPUが扱うもの:
- frame ID
- timestamp
- landmark coordinates
- confidence
- resource state
GPUが扱うもの:
- camera pixel data
- resize/crop output
- inference input
- optional inference output
- render target
NPU推論の場合
Camera → D3D11 GPU texture
              ↓
      shared/preprocessed input
              ↓
          NPU inference
              ↓
        landmark metadata
              ↓
           GPU render

NPUの場合は、GPUとNPUの間のメモリ共有が複雑になる可能性があります。

そのため、実測によっては次の方が速いこともあります。

GPU camera texture
    ↓ GPU crop/resize
small 256×256 tensor
    ↓ system/shared memory
NPU inference

1920×1080の画像全体をCPUへ戻すのは高コストですが、256×256の量子化済み入力だけをNPUへ渡すのであれば、転送コストは限定されます。

したがって、ゼロコピーを絶対条件にするのではなく、転送するデータ量を最小化するという設計も有効です。

結論

最も参考にすべき実装を目的別にまとめると、次の通りです。

目的	参考実装
カメラから描画までGPU完結	NVIDIA DeepStream
Windows GPUメモリ共有	OpenVINO GPU Remote Tensor
Windows NPUメモリ共有	OpenVINO NPU Remote Tensor
ONNX Runtimeを継続利用	Device Tensor + I/O Binding
GPU処理をグラフ化	MediaPipe GPU Calculator
NVIDIAで最大性能	CUDA + TensorRT + DeepStream
複数GPUベンダー対応	DirectML
Intel GPU/NPU中心	OpenVINO

現在のアプリに最も適した進め方は、まず次の段階です。

第1段階
Capture / Inference / Renderを分離
Latest-frame方式
固定shape
バッファ再利用

第2段階
前処理をD3D compute shaderへ移す
カメラ画像のCPU readbackを削除

第3段階
OpenVINO Remote Tensor
またはONNX Runtime I/O Binding

第4段階
GPU後処理
非同期command queue
fenceベースのresource lifecycle

特に最初から完全ゼロコピーを目指すより、CPU readback回数、GPU upload回数、各コピーのバイト数と待ち時間を計測し、最大のコピーから順に消す方が、実装負荷に対する効果が高くなります。

amd64 のqualcommチップで一番推奨されるフレームワークは、あなたが挙げてくれたもののうちどれですか？onnx runtime などは自前である程度管理する必要がありそうですが、nvidia streamなどはある程度抽象化されていそうな印象です（もちろんqualcomm製なので使用できませんが）。どう考えますか？
Worked for 21s

まず、Qualcomm Snapdragon X Elite／Plus搭載Windows PCは、通常 AMD64（x64）ではなくARM64 です。x64アプリをエミュレーション実行できるため混同しやすいですが、NPUをローカル利用する推論アプリは基本的にARM64ネイティブでビルドするべきです。ONNX Runtime QNNの公式パッケージも、ローカルQualcomm NPU推論はWindows ARM64を対象としています。

結論

現在の条件なら、第一候補は次です。

Windows ML + QNN Execution Provider

より具体的には、

Media Foundation / Windows Camera API
        ↓
D3D11・D3D12 video resource
        ↓
Windows ML
        ↓
QNN Execution Provider
        ↓
Qualcomm HTP / NPU

です。

単純に性能を最大化したい場合は、

ONNX Runtime + QNN Execution Providerを直接使用

さらに深いメモリ制御やQualcomm固有最適化まで行う場合は、

Qualcomm AI Runtime（QAIRT / QNN）を直接使用

という順になります。

選択肢	抽象度	性能制御	推奨用途
Windows ML + QNN EP	高い	中程度	Windows製品アプリ
ONNX Runtime + QNN EP	中程度	高い	今回のリアルタイム推論アプリ
QAIRT/QNN直接利用	低い	最大	Qualcomm専用・性能最優先
DirectML	中程度	GPU向け	Adreno GPU推論
MediaPipe	高い	限定的	姿勢推定ロジックの参考
DeepStream	高い	高い	NVIDIA専用なので対象外

私なら、最初はWindows ML + QNN EPを選びます。ただし、カメラ・推論・描画のパイプラインを細かく制御したくなった時点で、ONNX Runtime QNN EP直接利用へ移ります。

なぜWindows MLが第一候補か

2026年時点のWindows MLは、ONNX Runtimeを基盤として、CPU・GPU・NPU用のExecution Providerの選択、インストール、更新、ハードウェア適合をWindows側で管理する方向に進んでいます。

Windows 11 24H2以降では、QNN Execution ProviderをExecutionProviderCatalogから取得・登録でき、Qualcomm用QNN/QAIRTパッケージもWindows Update経由で更新されます。

つまり、アプリ側で次の管理を減らせます。

QNN DLLの配布
QNN SDKバージョンの整合
デバイス検出
Execution Providerの選択
CPUフォールバック
ベンダー別依存関係

Windows向けONNX Runtimeの公式ドキュメントも、Windows 11 24H2以降ではWindows MLを推奨しており、ハードウェアに応じたExecution Provider選択と依存関係の配布を自動化できると説明しています。カメラから得たWindows.Media.VideoFrameを直接ランタイムへ渡すリアルタイム推論も想定されています。

したがって、DeepStreamに近い「ベンダー固有SDKの詳細をある程度隠す」という意味では、Qualcomm Windows PCではWindows MLが最も近い位置にあります。

ただし、Windows MLはDeepStreamそのものではない

ここは重要です。

DeepStreamは次の全体を一つの映像処理フレームワークとして提供します。

camera / decoder
→ video buffer pool
→ color conversion
→ batching
→ inference
→ tracking
→ overlay
→ encode / display

一方、Windows MLが主に抽象化するのは、

model
→ execution provider selection
→ CPU / GPU / NPU inference

です。

つまりWindows MLは、次の部分までは自動で構築しません。

カメラバッファのプール管理
最新フレーム方式
GPU/NPU/描画間の同期
描画スレッド
Direct3Dリソースライフサイクル
inference結果と映像frameのtimestamp対応
overlay描画
dropped-frame policy
triple buffering

したがって、Qualcomm向けWindows環境には、現状、DeepStreamと完全に同じ位置にある統合映像AIフレームワークはありません。

おおまかな比較は次です。

NVIDIA:

DeepStream
 ├─ camera/video
 ├─ GPU buffer
 ├─ preprocess
 ├─ TensorRT
 ├─ tracker
 └─ renderer
Qualcomm Windows:

あなたのアプリ
 ├─ Media Foundation
 ├─ D3D11/D3D12
 ├─ Windows ML / ONNX Runtime
 │    └─ QNN EP
 │         └─ Qualcomm NPU
 └─ Direct2D/D3D renderer

Qualcomm環境では、映像パイプライン上位部分を自分で設計する必要があります。

Windows MLとONNX Runtime QNNの違い

両者は競合関係ではありません。

Windows ML
    ↓
ONNX Runtime
    ↓
QNN Execution Provider
    ↓
QAIRT / QNN
    ↓
Qualcomm HTP/NPU

Windows MLは、ONNX RuntimeとExecution Providerの管理をWindows向けに抽象化した上位層です。

Windows MLを選ぶ場合

利点：

Qualcomm以外のPCにも展開しやすい
QNN EPの配布と更新をWindows側に任せやすい
デバイス能力に応じたEP選択
Windows App SDKとの統合
将来的にIntel、AMD、NVIDIAへ展開しやすい
ARM64/x64の差をアプリ上位層で隠しやすい

欠点：

QNN固有オプションへ直接触れにくい
context binaryの詳細管理が限定される
backend選択やprofilingの細かい制御が難しい
-メモリ共有方式を完全に指定できるとは限らない
Windowsバージョンへの依存が強い
ONNX Runtime QNNを直接使う場合

利点：

backend_type=htpを明示できる
CPU fallbackを禁止できる
context binary cacheを管理できる
profilingを細かく取得できる
HTP、GPU、CPU backendを明示できる
SessionOptionsやI/O Bindingを制御できる
C++ネイティブアプリへの統合が明確

QNN EPは、ONNXグラフをQualcomm AI Engine DirectのQNNグラフへ変換し、HTP backendを指定するとNPUへオフロードします。公式にはhtpがデフォルトで、gpuやcpuも選択可能です。

欠点：

QNN EPのバージョン管理が必要
モデル量子化が必要になることが多い
未対応operatorの確認が必要
CPU fallbackの有無を検証する必要がある
bufferとthreadの管理はアプリ責任
カメラ入力と描画は別途実装
あなたのアプリにはどちらが向いているか

今回のような、

リアルタイムカメラ
手や姿勢の推論
カメラ上へのoverlay
Direct3D/Direct2Dによるネイティブ描画
latencyを細かく測定
CPU/GPU/NPU間コピーを削減

というアプリでは、最終的にはONNX Runtime QNN EP直接利用の方が合っています。

Windows MLは初期実装や配布には優れていますが、あなたが重視しているのは単なるNPU利用ではなく、

capture
→ resource ownership
→ preprocess
→ inference
→ result synchronization
→ rendering

の制御だからです。

推奨する実装方針は、Windows MLかONNX Runtimeかを推論抽象層の裏側で差し替えられるようにすることです。

class IInferenceBackend {
public:
    virtual ~IInferenceBackend() = default;

    virtual bool Initialize(const ModelConfig& config) = 0;

    virtual InferenceTicket Submit(
        const InferenceInput& input
    ) = 0;

    virtual bool TryGetResult(
        InferenceTicket ticket,
        PerceptionResult& result
    ) = 0;
};

実装は次のようにします。

IInferenceBackend
 ├─ WindowsMlBackend
 ├─ OnnxQnnBackend
 ├─ OnnxDirectMlBackend
 └─ CpuBackend

これなら、最初はWindows MLで実装し、必要になった部分だけQNN直接利用へ移せます。

QNN EPは十分に抽象化されているか

かなり抽象化されています。

QNN SDKを直接使う場合は、本来次のような操作が必要です。

QNN backend creation
QNN device creation
QNN context creation
QNN graph creation
tensor registration
graph finalization
execution
profiling
resource destruction

ONNX Runtime QNN EPを使うと、これらの大半が、

Ort::SessionOptions sessionOptions;

std::unordered_map<std::string, std::string> options{
    {"backend_type", "htp"}
};

sessionOptions.AppendExecutionProvider("QNN", options);

Ort::Session session(
    env,
    modelPath,
    sessionOptions
);

程度へ集約されます。

QNN EPは内部で、ONNXモデルからQNNグラフを構築し、指定されたQualcomm accelerator backend上で実行します。

したがって、ONNX Runtimeを使うからといって、NPUのコマンドキューやHexagon内部メモリをすべて自分で管理するわけではありません。

自分で管理するのは主に上位層です。

アプリ側が管理:
- 入力・出力buffer
- frame lifetime
- thread
- async scheduling
- model session lifetime
- pre/postprocess
- rendererとの同期
QNN EP側が管理:
- ONNX→QNN graph変換
- operator mapping
- NPU graph compilation
- NPU memory allocationの多く
- backend execution
- device-specific kernels

つまり、

ONNX Runtime QNN EPは、DeepStreamより低い抽象度だが、QNN SDK直接利用よりかなり高い抽象度

です。

GPUとNPUのどちらを使うべきか

Qualcomm Snapdragon Xでは、リアルタイム姿勢・手推論はまずNPUを推奨します。

NPU:
- 推論
- 低消費電力
- 継続実行
- UI描画と競合しにくい

Adreno GPU:
- カメラ画像処理
- resize / crop
- color conversion
- overlay
- final composition

役割分担は次が妥当です。

CPU
 ├─ scheduling
 ├─ timestamps
 ├─ state management
 └─ small pose metadata

Adreno GPU
 ├─ camera texture
 ├─ color conversion
 ├─ crop / resize
 ├─ camera rendering
 └─ skeleton overlay

Hexagon NPU / HTP
 └─ neural inference

QNN EPはHTP/NPUだけでなくGPU backendも選べます。同じクライアントコードでbackendを切り替えられる設計になっています。QualcommもHTP backendとAdreno GPU backendをsession作成時のオプションで切り替える方式を示しています。

ただし、GPU推論とGPU描画を同時に行うと、Adreno GPU上でリソース競合が起きます。

GPU compute:
neural inference

GPU graphics:
camera composition
overlay
swap-chain rendering

そのため、通常は、

NPU inference + GPU rendering

の方が、アプリ全体として安定します。

DirectMLを第一候補にしない理由

DirectMLはAdreno GPUで動作しますが、NPUを主目的とする場合の第一候補ではありません。

DirectMLはDirectX 12対応GPU全般を対象にしたGPU推論APIです。Qualcommを含む広いGPUをサポートしますが、現在はmaintenance/sustained-engineeringの位置づけで、WindowsではWindows MLが推奨経路になっています。

DirectMLを使うべきケースは次です。

モデルがQNN NPUで動作しない
QNN未対応operatorが多い
FP16 GPUモデルをそのまま使いたい
D3D12 resourceとの統合を最優先する
GPU上で前処理・推論・後処理・描画を完結させたい

一方、長時間のリアルタイム推論では、

QNN HTP/NPU

を第一候補にし、

QNN GPUまたはDirectML

をfallbackにするのが合理的です。

推奨する最終構成
ARM64 native Windows application

Media Foundation
        ↓
D3D11 camera texture
        ├──────────────────→ Render thread
        │                       ↓
        │                  Direct2D / D3D11
        │
        ↓
GPU preprocess
crop / resize / format conversion
        ↓
small fixed-shape inference buffer
        ↓
ONNX Runtime
        ↓
QNN Execution Provider
        ↓
HTP / NPU
        ↓
PoseResult
        ↓
Render thread
        ↓
DXGI flip-model swap chain

推論backendの優先順位は次です。

1. QNN HTP / NPU
2. QNN Adreno GPU
3. DirectML GPU
4. ONNX Runtime CPU

実装段階としては、

ARM64ネイティブ化
ONNX Runtime QNN EPでNPU推論を成立
fixed-shape・INT8モデルを用意
CPU fallbackを禁止して、本当にNPU上で全グラフが動くか検証
context binary cacheを有効化
capture・inference・renderを別スレッド化
CPUへのフルフレームreadbackを除去
必要ならWindows MLへ配布管理を移す

の順がよいです。

要するに、DeepStream相当の完全統合フレームワークはQualcomm Windowsにはまだありません。しかし推論部分に限れば、Windows ML → ONNX Runtime → QNN EP → NPUによって相当程度抽象化できます。今回のアプリでは、Windows MLを外側の配布・デバイス抽象層、ONNX Runtime QNN EPを性能制御層として考えるのが適切です。

その場合NPU/GPU/CPUの間の情報共有のメモリコピーやstreamのオーバーヘッドはどのように管理することになりますか？また、管理できるのでしょうか？
Worked for 20s

管理はできます。ただし、ONNX Runtime＋QNN EPで管理できる範囲には限界があります。

最も重要な点は次です。

Snapdragon XはCPU・GPU・NPUが同じ物理DRAMを共有するSoCですが、同じDRAMだから自動的にゼロコピーになるわけではありません。

各デバイスは異なるアドレス空間、キャッシュ、ドライバー、実行キューを持つため、次が必要です。

共有可能なバッファの確保
各デバイスへのマッピング
バッファ所有権の管理
キャッシュ整合性
実行完了の同期
使用中バッファの上書き防止
管理レベルは3段階あります
1. Windows ML＋QNN EP

最も抽象度が高い構成です。

Application
   ↓
Windows ML / ONNX Runtime
   ↓
QNN Execution Provider
   ↓
Qualcomm driver / HTP runtime
   ↓
NPU

この場合、NPU内部のメモリ割り当て、NPUへのRPC、実行キュー、演算用一時バッファなどは、基本的にQNN EPとQualcommランタイムが管理します。Windows MLもONNX Runtimeと各社のExecution Providerを組み合わせる層であり、ハードウェア固有の実装を隠します。

一方、アプリが管理できるのは主に次です。

CPU上の入力・出力バッファ
推論要求を送るタイミング
キャプチャ、推論、描画スレッド
バッファプール
フレーム破棄方針
推論結果とフレームの対応
QNN EPの各種設定

この構成では、NPUの内部queueやstreamをCUDAのように直接操作することは基本的にできません。

2. ONNX Runtime＋QNN EP

もう少し制御できます。

QNN EPには、HTP共有メモリアロケータを有効化する設定があります。

providerOptions["backend_type"] = "htp";
providerOptions["enable_htp_shared_memory_allocator"] = "1";

この設定により、ONNX Runtimeの入出力TensorをHTPから直接アクセス可能な共有メモリへ置けます。QNN EPの公式設定には、enable_htp_shared_memory_allocatorが明示されており、共有メモリ経由で不要なコピーを減らせます。

概念的には次です。

通常:

CPU ordinary buffer
    ↓ copy
HTP-accessible buffer
    ↓
NPU
共有allocator:

CPU-visible and HTP-accessible buffer
    ↓
NPU

QualcommのHTP Shared Bufferは、CPUなどの処理ドメインとHTP間で共有アクセスできるバッファを用い、コピーを除去する目的の機能です。

ただし、ここで共有されるのは基本的に、

CPU側のTensorバッファとNPU側

です。

次のようなD3D11カメラテクスチャを、そのままQNN EPへ渡せるとは限りません。

ID3D11Texture2D
    ↓
QNN HTP shared allocator

QNN EPの公開APIには、CUDA EPのように任意のGPUリソースをNPU Tensorとして直接インポートする、一般的なWindows向け相互運用APIは明確には提供されていません。

したがって現実的には次の構造になります。

Camera D3D11 texture
        ↓ GPU crop/resize/color conversion
small inference input
        ↓ copy or shared staging buffer
QNN HTP shared memory
        ↓
NPU inference

フルHD画像全体ではなく、たとえば256 × 256 × 3の入力だけを転送することで、コピーコストを小さくします。

3. QAIRT／QNN APIを直接使用

最大限制御する構成です。

Application
   ↓
QNN API
   ↓
QNN HTP backend
   ↓
NPU

QNN直接利用では、次をより明示的に扱えます。

shared bufferの確保と登録
tensorとbufferの関連付け
graphの実行
非同期実行
signalと完了通知
queue depth
context・graphの優先度
VTCM設定
performance mode
profiling

QNNには同期実行のQnnGraph_execute()と、非同期実行のQnnGraph_executeAsync()があります。非同期実行ではグラフがFIFOで実行キューへ入れられます。

このレベルなら、

Buffer A: GPU preprocessing
Buffer B: NPU inference
Buffer C: CPU postprocess / GPU render

を明示的に回す構成が可能です。

ただし、D3D11／D3D12リソースとQNN shared bufferを完全に同一オブジェクトとして共有できるかは、使用するQAIRTバージョン、ドライバー、Windows側の対応APIに依存します。NVIDIA CUDAのように統一された公開interop経路が常に使えるとは考えない方が安全です。

CPU・GPU・NPU間で実際に何を共有するか

推奨構成では、画像全体を3デバイス間で回しません。

Camera
  ↓
GPU texture: 1920×1080 NV12
  │
  ├──→ GPU rendering
  │
  └──→ GPU crop/resize
           ↓
       256×256 input
           ↓
        NPU inference
           ↓
       landmarks only
           ↓
         CPU/GPU

データ量の概算は次です。

1920×1080 BGRA:
約8.3 MB/frame

256×256 RGB uint8:
約192 KB/frame

33 landmarks × 4 values × float:
約528 bytes/frame

したがって、コピーを完全にゼロにできなくても、

8.3 MBを転送

する代わりに、

192 KBだけNPUへ転送
528 bytesだけ結果として戻す

なら、実用上は十分小さい可能性があります。

Snapdragon Xでの現実的な役割分担
GPU:
- カメラテクスチャ保持
- NV12→RGB
- crop
- resize
- 描画
- overlay

NPU:
- 推論本体

CPU:
- scheduling
- timestamp管理
- ROI更新
- 小さい推論結果の処理

この分担では、GPUとNPUの間に1回だけ小さい入力転送が発生します。

Streamの管理

ここでいうstreamには、少なくとも3種類あります。

1. Camera stream
2. GPU command queue
3. NPU execution queue

これらは同じstreamではありません。

カメラ側

Media Foundationからフレームが到着します。

Camera callback
    ↓
Frame N arrives

ここではフレームをFIFOへ無制限に積まず、最新値スロットか小さいring bufferに入れます。

struct CameraFrame {
    uint64_t frameId;
    int64_t timestamp;
    ComPtr<ID3D11Texture2D> texture;
};
GPU側

GPU前処理はD3D11 immediate contextまたはD3D12 command queueへ投入します。

D3D command:
crop + resize + color conversion

完了を確認するには、D3D11 queryやD3D12 fenceを使います。

GPU preprocess submitted
       ↓
GPU fence signaled
       ↓
NPU input is ready
NPU側

ONNX Runtime QNN EPを使う場合、NPU内部のqueueやRPCはQNN EPが管理します。

アプリからは通常、

session.Run(...);

という呼び出しになります。

この呼び出しを推論専用スレッドで実行すれば、UIスレッドを止めずに済みます。

UI thread
Capture thread
Inference thread

ただし、これはアプリレベルで非同期にしているだけで、QNN内部queueを直接操作しているわけではありません。

QAIRTを直接使えば、QnnGraph_executeAsync()でNPU実行を明示的にenqueueできます。

推奨する同期方法

CPUで全デバイスの完了を毎回待つ構成は避けます。

悪い例です。

Capture
↓ wait
GPU preprocess
↓ wait
NPU inference
↓ wait
CPU postprocess
↓ wait
Render

これでは各処理が直列になります。

推奨構成です。

Time →
Frame N:     GPU preprocess → NPU inference → result
Frame N+1:          GPU preprocess → NPU inference
Frame N+2:                 capture / render

最低3組のバッファを用意します。

struct PipelineSlot {
    CameraTexture cameraTexture;
    PreprocessBuffer inferenceInput;
    InferenceOutput inferenceOutput;

    uint64_t frameId;
    PipelineState state;
};

状態は次のようにします。

FREE
  ↓
CAPTURING
  ↓
GPU_PREPROCESSING
  ↓
READY_FOR_NPU
  ↓
NPU_RUNNING
  ↓
RESULT_READY
  ↓
FREE

GPU fenceとNPU完了通知を使って状態を遷移させます。

ONNX Runtime QNN EPだけを使う場合は、次の簡略構成になります。

GPU fence complete
        ↓
Inference thread wakes
        ↓
session.Run()
        ↓
Result published

QAIRT直接利用なら、

GPU fence complete
        ↓
QnnGraph_executeAsync()
        ↓
QNN completion signal
        ↓
Result published

とできます。

ONNX Runtime QNN EPで管理できる項目

かなりの項目は設定できます。

backend_type
htp_performance_mode
rpc_control_latency
vtcm_mb
qnn_context_priority
graph finalization optimization
shared memory allocator
profiling
CPU fallback
context binary cache

QNN EPでは、HTP performance mode、RPC latency、VTCMサイズ、context priorityなどを指定できます。

たとえばリアルタイム用途では、

providerOptions["backend_type"] = "htp";
providerOptions["htp_performance_mode"] = "sustained_high_performance";
providerOptions["qnn_context_priority"] = "high";
providerOptions["enable_htp_shared_memory_allocator"] = "1";

などが候補になります。

ただし、burstやhigh_performanceを常時使用すると、電力消費と発熱が増えます。長時間カメラアプリならbalancedまたはsustained_high_performanceを実測比較するべきです。

管理できない、または見えにくい部分

ONNX Runtime QNN EPでは、次は基本的にランタイム内部です。

HTP内部の演算schedule
DSP/NPUの命令queue詳細
内部scratch buffer配置
各operatorのVTCM割り当て
NPUキャッシュの細かい制御
NPU RPC実装
ドライバー内部のコピー
GPUとNPU間の物理的な転送経路
各tensorの内部layout変換

したがって、

共有allocatorを有効にしたので全コピーがなくなった

とは判断できません。

たとえば次の変換が内部で残る可能性があります。

RGB HWC uint8
    ↓
NHWC quantized tensor

または

uint8
    ↓
uint16 / packed HTP layout

QNN EPでは、入出力の量子化・逆量子化をCPU EPへオフロードする設定もあり、デフォルトでは有効です。つまりモデルのI/O型によってはCPU処理が挿入されます。

このため、NPU向けモデルの入出力を最初から量子化形式に合わせることが重要です。

実装上の推奨

あなたのケースでは、最初からQAIRT直接利用へ進む必要はありません。

まずは次です。

Media Foundation
    ↓ D3D11 texture
GPU preprocessing
    ↓ fixed-size uint8 buffer
ONNX Runtime QNN EP
    ↓ HTP shared memory allocator
NPU inference
    ↓ small landmarks
GPU rendering

そして以下を計測します。

T_capture
T_gpu_preprocess
T_gpu_to_npu_input
T_npu_execution
T_output_read
T_render
End-to-end latency

QNN EPにはbasic、detailed、optraceのprofiling levelと、CSVへの出力機能があります。WindowsではETWによる動的profilingも利用できます。

この計測でGPU→NPU入力やNPU enqueueが明確なボトルネックになった場合にのみ、QAIRT直接利用を検討します。

最終的な判断
項目	Windows ML	ORT＋QNN EP	QAIRT直接
CPU–NPU共有メモリ	間接的	一部制御可能	詳細制御可能
GPU–NPU直接共有	限定的	限定的	環境依存で高度
NPU非同期queue	非公開	基本的に非公開	制御可能
NPU priority	EP次第	設定可能	詳細制御可能
buffer pool	アプリ管理	アプリ管理	アプリ管理
GPU fence	アプリ管理	アプリ管理	アプリ管理
NPU completion signal	抽象化	抽象化	明示的
実装難易度	低	中	高

結論として、管理は可能ですが、ONNX Runtime QNN EPでは「デバイス内部を直接管理する」のではなく、「コピーが少なくなるように入出力とパイプラインを設計する」形です。

今回のアプリでは、まずQNN EPのHTP shared allocator、固定入出力バッファ、3重バッファリング、推論専用スレッドを使うのが妥当です。GPUテクスチャとNPUメモリを完全に同一バッファ化したい、あるいはNPU queueを直接制御したい段階で、QAIRT/QNN APIへ下げる必要があります。