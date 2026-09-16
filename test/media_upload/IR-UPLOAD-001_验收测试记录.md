# IR-UPLOAD-001 图片云端上传触发链路验收测试记录

- 日期：2026-08-27
- 板端：ubuntu@10.118.87.41（aarch64）
- 被测：aibox-media-upload（上传）+ aibox-data_manager（alarm/media join）+ 本地 mock S3
- 上游：pipeline(webrtc_start6 6路) + rule_engine

---

## 1. 总结论

| 项 | 结论 |
|---|---|
| Phase A 合成注入：alarm_ready 触发 | 通过（1 条，event_id/alarm_id 匹配） |
| Phase A 合成注入：media-upload 上传 + event.media.uploaded | 通过（1 条，cloud_url 非空，mock S3 收到 1 次 PUT） |
| Phase B 全链路真实（60s）：上传被成功触发 | 通过（49 次上传 / 54 次 PUT） |
| 触发率（alarm → 上传） | 约 0.6%（49/8230），见 §4 |

## 2. 触发链路（已实测验证）

rule_engine → alarm.event(ALARM_PIPELINE) → data_manager save_alarm
pipeline → event.stream.media_ready → data_manager save_media_meta + get_alarm_id(event_id) → 命中则发 event.datamgr.alarm_ready
media-upload(consumer on ALARM_READY_PIPELINE) → get_media_meta → 读 jpg → S3 PUT → mark_uploaded → event.media.uploaded

## 3. 兼容/差异分析

- 关联键 = event_id：alarm.event 的 event_id 取帧内第一个检测；media_ready 对该帧每个检测各发一条（同 file_path）。实测 join 正常。
- data_manager 是必需中间件（join + alarm_ready + get_media_meta/mark_uploaded 两个 RPC）。
- 上游两个触发解耦：pipeline 存图（类别变化+holdoff）与 rule_engine 告警（规则）不重合 → 只有「该帧既有 alarm 又存了图」才触发上传，故 alarm→上传 触发率约 0.6%。
- media-upload 的 s3 配置强制 access/secret/bucket 非空；本测试用本地 mock S3（SigV4 不校验）。

## 4. Phase A 结果（合成注入）

| 指标 | 值 |
|---|---|
| alarm_ready 数 | 1 |
| event.media.uploaded 数 | 1 |
| 匹配的 event_id / alarm_id | test-evt-1 / test-alm-1 |
| cloud_url | http://127.0.0.1:19000/test-bucket/test-evt-1.jpg |
| mock S3 PUT 次数 | 1 |

## 5. Phase B 结果（全链路真实 60s）

| 指标 | 值 |
|---|---|
| detection.frame（pipeline 发布） | 4090 |
| alarm.event（data_manager 落库） | 8230 |
| alarm_ready 数 | 49 |
| event.media.uploaded 数 | 49 |
| mock S3 PUT 次数 | 54（含 5 次失败后 NAK 重传） |
| alarm → 上传 触发率 | 49 / 8230 ≈ 0.6% |

## 6. 复现命令（板端）

    cd /mnt/workspace/8_20/IoT_pipeline0820
    bash test/media_upload/run_upload_e2e.sh synth
    bash test/media_upload/run_upload_e2e.sh live 60

产物在 output/upload_e2e/（verifier_summary.txt / data_manager.log / media_upload.log / mock_s3.log 等）。

## 7. 明确未做
- 不校验 S3 SigV4 签名正确性（mock S3 不校验）。
- 不验证真实云存储/真实凭据。
- 不验证缩略图（ffmpeg）与 media 淘汰。
