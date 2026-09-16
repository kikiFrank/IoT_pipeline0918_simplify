# 图片云端上传触发链路测试（IR-UPLOAD-001）

验证「alarm 事件 + 图片保存成功发 media_ready」两条件满足后，aibox-media-upload 能否被成功触发（走通 alarm.event/media_ready → data_manager join → alarm_ready → S3 PUT → event.media.uploaded）。

## 文件
- mock_s3.py：本地 mock S3（接收 PUT 返回 200，计数 + 落盘）
- upload_injector.cpp / upload_verifier.cpp：合成注入器 / 校验器
- CMakeLists.txt：编译两个 helper
- data_manager.toml / media-upload.toml：两个微服务的测试配置（独立 db / 本地 mock S3）
- run_upload_e2e.sh：编排脚本（synth 合成 / live 真实两种模式）
- IR-UPLOAD-001_验收测试记录.md：报告

## 前置（板端）
1. nats-server 4222 + JetStream 启用
2. data_manager 与 media-upload 二进制（media-upload 若缺失用 cargo 重建：~/.cargo/bin/cargo build -p aibox-media-upload）
3. 已编译 upload_injector / upload_verifier

## 运行（板端）
    bash test/media_upload/run_upload_e2e.sh synth   # Phase A 合成注入
    bash test/media_upload/run_upload_e2e.sh live 60 # Phase B 全链路真实

## 说明
- 不改 factory_iot 源码，只运行其二进制。
- 用独立 device_sn + 独立 db_path + 独立 event_id（test-evt-*）隔离，不碰生产库与既有数据。
- 全程 SIGTERM。
