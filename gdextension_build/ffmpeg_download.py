import os
import shutil
import tarfile
import zipfile
import urllib.request

import SCons

FFMPEG_DOWNLOAD_WIN64 = "https://github.com/Elitenv/FFmpeg-Builds/releases/download/autobuild-2026-06-29-17-14/ffmpeg-n7.1.5-1-g7d0e842004-win64-lgpl-7.1.zip"
FFMPEG_DOWNLOAD_LINUX64 = "https://github.com/Elitenv/FFmpeg-Builds/releases/download/autobuild-2026-06-29-17-14/ffmpeg-n7.1.5-1-g7d0e842004-linux64-lgpl-7.1.tar.xz"
ffmpeg_versions = {
    "avcodec": "62",
    "avfilter": "11",
    "avformat": "62",
    "avutil": "60",
    "swresample": "6",
    "swscale": "9",
}


def get_ffmpeg_install_targets(env, target_dir):
    if env["platform"] == "linuxbsd" or env["platform"] == "linux":
        return [os.path.join(target_dir, f"lib{lib}.so.{version}") for lib, version in ffmpeg_versions.items()]
    elif env["platform"] == "android":
        return [os.path.join(target_dir, f"lib{lib}.so") for lib in ffmpeg_versions]
    elif env["platform"] == "macos":
        return [os.path.join(target_dir, f"lib{lib}.dylib") for lib in ffmpeg_versions]
    else:
        return [os.path.join(target_dir, f"{lib}-{version}.dll") for lib, version in ffmpeg_versions.items()]


def get_ffmpeg_install_sources(env, source_dir):
    if env["platform"] in ("linuxbsd", "linux", "android"):
        return [os.path.join(source_dir, f"lib/lib{lib}.so") for lib in ffmpeg_versions]
    elif env["platform"] == "macos":
        return [os.path.join(source_dir, f"lib/lib{lib}.dylib") for lib in ffmpeg_versions]
    else:
        return [os.path.join(source_dir, f"bin/{lib}-{version}.dll") for lib, version in ffmpeg_versions.items()]


def get_download_url(env):
    if env["platform"] == "linuxbsd" or env["platform"] == "linux":
        FFMPEG_DOWNLOAD_URL = FFMPEG_DOWNLOAD_LINUX64
    else:
        FFMPEG_DOWNLOAD_URL = FFMPEG_DOWNLOAD_WIN64
    return FFMPEG_DOWNLOAD_URL


def rewrite_subfolder_paths(tf, common_path):
	"""重写 tar 内成员路径，去掉公共前缀层"""
	common_path_length = len(common_path)
	for member in tf.getmembers():
		if member.path.startswith(common_path):
			member.path = member.path[common_path_length:]
			yield member


def download_ffmpeg(target, source, env):
	dst = ""
	if isinstance(target[0], str):
		dst = os.path.dirname(target[0])
	else:
		dst = os.path.dirname(target[0].get_path())
	if os.path.exists(dst):
		shutil.rmtree(dst)

	FFMPEG_DOWNLOAD_URL = get_download_url(env)

	local_filename, headers = urllib.request.urlretrieve(FFMPEG_DOWNLOAD_URL)

	# 支持 .tar.xz 和 .zip 格式
	if local_filename.endswith(".zip"):
		with zipfile.ZipFile(local_filename, "r") as zf:
			# 取第一层目录作为 common_path
			names = zf.namelist()
			common_path = os.path.commonpath(names) + "/"
			for name in names:
				if name.startswith(common_path):
					dest = os.path.join(dst, name[len(common_path):])
					os.makedirs(os.path.dirname(dest), exist_ok=True)
					if not name.endswith("/"):
						with open(dest, "wb") as f:
							f.write(zf.read(name))
	else:
		with tarfile.open(local_filename, mode="r") as f:
			common_path = os.path.commonpath(f.getnames()) + "/"
			f.extractall(dst, members=rewrite_subfolder_paths(f, common_path))
	os.remove(local_filename)


def _ffmpeg_emitter(target, source, env):
    dst = ""
    if isinstance(target[0], str):
        dst = os.path.dirname(target[0])
    else:
        dst = os.path.dirname(target[0].get_path())
    target += get_ffmpeg_install_sources(env, dst)
    if env["platform"] == "windows":
        target += [os.path.join(dst, f"lib/{lib}.lib") for lib, version in ffmpeg_versions.items()]
    else:
        target += [os.path.join(dst, f"lib/lib{lib}.so") for lib, version in ffmpeg_versions.items()]

    emitter_headers = [
        "libavcodec/codec.h",
        "libavcodec/avcodec.h",
        "libavutil/frame.h",
        "libavformat/avformat.h",
        "libavformat/avio.h",
        "libswresample/swresample.h",
        "libswscale/swscale.h",
    ]

    target += [os.path.join(dst, "include/" + x) for x in emitter_headers]

    return target, source


def ffmpeg_download_builder(env, target, source):
    bkw = {
        "action": env.Run(download_ffmpeg),
        "target_factory": env.fs.Entry,
        "source_factory": env.fs.Entry,
        "emitter": _ffmpeg_emitter,
    }

    bld = SCons.Builder.Builder(**bkw)
    return bld(env, target, source)


def ffmpeg_install(env, target, source):
    return env.InstallAs(get_ffmpeg_install_targets(env, target), get_ffmpeg_install_sources(env, source))
