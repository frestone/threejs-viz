// 拖动结束后，仅目标高清与保留缩略图来自同一源图像时才切换。
export function shouldReleaseThumbnail(
  thumbnailSeq: number,
  highResolutionSeq: number,
): boolean { return thumbnailSeq === highResolutionSeq; }