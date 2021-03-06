#include "Frame.h"

namespace PoseTracking
{
	//下一个生成的帧的ID,这里是初始化类的静态成员变量
	long unsigned int Frame::nNextId = 0;

	Frame::Frame(const Frame* frame):mbInitialComputations(frame->mbInitialComputations), mnMinX(frame->mnMinX), mnMinY(frame->mnMinY), mnMaxX(frame->mnMaxX),
		mnMaxY(frame->mnMaxY),mfGridElementWidthInv(frame->mfGridElementWidthInv), mfGridElementHeightInv(frame->mfGridElementHeightInv), mnId(frame->mnId),
		mvpMapPoints(frame->mvpMapPoints), mvKeys(frame->mvKeys), mDescriptors(frame->mDescriptors.clone()), mvbOutlier(frame->mvbOutlier),
		mpReferenceKF(frame->mpReferenceKF), mnScaleLevels(frame->mnScaleLevels), mfScaleFactor(frame->mfScaleFactor), mfLogScaleFactor(frame->mfLogScaleFactor),
		mvScaleFactors(frame->mvScaleFactors), mvInvScaleFactors(frame->mvInvScaleFactors), mvLevelSigma2(frame->mvLevelSigma2), mvInvLevelSigma2(frame->mvInvLevelSigma2)
	{
		mTimeStamp = frame->mTimeStamp;

		//新的帧设置Pose的初始值，便于后续优化
		if (!frame->mTcw.empty())
			SetPose(frame->mTcw);

		//逐个复制，其实这里也是深拷贝
		//这里没有使用前面的深拷贝方式的原因可能是mGrid是由若干vector类型对象组成的vector，
		//但是自己不知道vector内部的源码不清楚其赋值方式，在第一维度上直接使用上面的方法可能会导致
		//错误使用不合适的复制函数，导致第一维度的vector不能够被正确地“拷贝”
		for (int i = 0; i < FRAME_GRID_COLS; i++)
			for (int j = 0; j < FRAME_GRID_ROWS; j++)
				mGrid[i][j] = frame->mGrid[i][j];
	}

	Frame::Frame(const cv::Mat &imGray, const double &timeStamp, orbDetector* extractor, const cv::Mat &K, const cv::Mat &Distort):mTimeStamp(timeStamp)
	{
		// Step 1 帧的ID 自增
		mnId = nNextId++;

		// Step 2 计算图像金字塔的参数 
		//获取图像金字塔的层数
		mnScaleLevels = extractor->GetLevels();
		//这个是获得层与层之前的缩放比
		mfScaleFactor = extractor->GetScaleFactor();
		//计算上面缩放比的对数, NOTICE log=自然对数，log10=才是以10为基底的对数 
		mfLogScaleFactor = log(mfScaleFactor);
		//获取每层图像的缩放因子
		mvScaleFactors = extractor->GetScaleFactors();
		//同样获取每层图像缩放因子的倒数
		mvInvScaleFactors = extractor->GetInverseScaleFactors();
		//高斯模糊的时候，使用的方差
		mvLevelSigma2 = extractor->GetScaleSigmaSquares();
		//获取sigma^2的倒数
		mvInvLevelSigma2 = extractor->GetInverseScaleSigmaSquares();

		std::vector<cv::KeyPoint> vKeys;

		//这里使用了仿函数来完成，重载了括号运算符 ORBextractor::operator() 
		(*extractor)(imGray,   //待提取特征点的图像
			vKeys,             //输出变量，用于保存提取后的特征点     
			mDescriptors);     //输出变量，用于保存特征点的描述子

		if (vKeys.size() == 0) return;

		//用OpenCV的矫正函数、内参对提取到的特征点进行矫正 
		UndistortKeyPoints(vKeys, K, Distort);

		//mvKeys中保存的是左图像中的特征点，这里是获取左侧图像中特征点的个数
		int N = mvKeys.size();

		//初始化本帧的地图点
		mvpMapPoints = std::vector<MapPoint*>(N, static_cast<MapPoint*>(NULL));
		//记录地图点是否为外点，初始化均为外点false
		mvbOutlier = std::vector<bool>(N, false);

		//计算去畸变后图像边界，将特征点分配到网格中。这个过程一般是在第一帧或者是相机标定参数发生变化之后进行
		if (mbInitialComputations)
		{
			//计算去畸变后图像的边界
			ComputeImageBounds(imGray, K, Distort);

			//表示一个图像像素相当于多少个图像网格列（宽）
			mfGridElementWidthInv = (float)(FRAME_GRID_COLS) / (float)(mnMaxX - mnMinX);
			//表示一个图像像素相当于多少个图像网格行（高）
			mfGridElementHeightInv = (float)(FRAME_GRID_ROWS) / (float)(mnMaxY - mnMinY);

			//特殊的初始化过程完成，标志复位
			mbInitialComputations = false;
		}

		AssignFeaturesToGrid();
	}

	/**
	 * @brief 将提取的ORB特征点分配到图像网格中
	 *
	 */
	void Frame::AssignFeaturesToGrid()
	{
		int N = mvKeys.size();
		// Step 1  给存储特征点的网格数组 Frame::mGrid 预分配空间
		// FRAME_GRID_COLS = 64，FRAME_GRID_ROWS=48
		int nReserve = std::ceil(1.0f*N / (FRAME_GRID_COLS*FRAME_GRID_ROWS));
		//开始对mGrid这个二维数组中的每一个vector元素遍历并预分配空间
		for (unsigned int i = 0; i < FRAME_GRID_COLS; i++)
			for (unsigned int j = 0; j < FRAME_GRID_ROWS; j++)
				mGrid[i][j].reserve(nReserve);

		// Step 2 遍历每个特征点，将每个特征点在mvKeysUn中的索引值放到对应的网格mGrid中
		for (int i = 0; i < N; i++)
		{
			//从类的成员变量中获取已经去畸变后的特征点
			const cv::KeyPoint &kp = mvKeys[i];

			//存储某个特征点所在网格的网格坐标，nGridPosX范围：[0,FRAME_GRID_COLS], nGridPosY范围：[0,FRAME_GRID_ROWS]
			int nGridPosX, nGridPosY;
			// 计算某个特征点所在网格的网格坐标，如果找到特征点所在的网格坐标，记录在nGridPosX,nGridPosY里，返回true，没找到返回false
			if (PosInGrid(kp, nGridPosX, nGridPosY))
				//如果找到特征点所在网格坐标，将这个特征点的索引添加到对应网格的数组mGrid中
				mGrid[nGridPosX][nGridPosY].push_back(i);
		}
	}

	// 设置相机姿态
	void Frame::SetPose(cv::Mat Tcw)
	{
		mTcw = Tcw.clone();
		UpdatePoseMatrices();
	}

	//根据Tcw计算mRcw、mtcw和mRwc、mOw
	void Frame::UpdatePoseMatrices()
	{
		// mOw：    当前相机光心在世界坐标系下坐标
		// mTcw：   世界坐标系到相机坐标系的变换矩阵
		// mRcw：   世界坐标系到相机坐标系的旋转矩阵
		// mtcw：   世界坐标系到相机坐标系的平移向量
		// mRwc：   相机坐标系到世界坐标系的旋转矩阵

		//从变换矩阵中提取出旋转矩阵
		//注意，rowRange这个只取到范围的左边界，而不取右边界
		mRcw = mTcw.rowRange(0, 3).colRange(0, 3);

		// mRcw求逆即可
		mRwc = mRcw.t();

		// 从变换矩阵中提取出旋转矩阵
		mtcw = mTcw.rowRange(0, 3).col(3);

		// mTcw 求逆后是当前相机坐标系变换到世界坐标系下，对应的光心变换到世界坐标系下就是 mTcw的逆 中对应的平移向量
		mOw = -mRcw.t()*mtcw;
	}

	/**
	 * @brief 用内参对特征点去畸变，结果报存在mvKeysUn中
	 *
	 */
	void Frame::UndistortKeyPoints(const std::vector<cv::KeyPoint>& vKeys, const cv::Mat &K, const cv::Mat& Distort)
	{
		// Step 1 如果第一个畸变参数为0，不需要矫正。第一个畸变参数k1是最重要的，一般不为0，为0的话，说明畸变参数都是0
		//变量mDistCoef中存储了opencv指定格式的去畸变参数，格式为：(k1,k2,p1,p2,k3)
		if (Distort.at<float>(0) == 0.0)
		{
			mvKeys = vKeys;
			return;
		}

		// Step 2 如果畸变参数不为0，用OpenCV函数进行畸变矫正
		// N为提取的特征点数量，为满足OpenCV函数输入要求，将N个特征点保存在N*2的矩阵中
		int N = vKeys.size();
		cv::Mat mat(N, 2, CV_32F);
		//遍历每个特征点，并将它们的坐标保存到矩阵中
		for (int i = 0; i < N; i++)
		{
			//然后将这个特征点的横纵坐标分别保存
			mat.at<float>(i, 0) = vKeys[i].pt.x;
			mat.at<float>(i, 1) = vKeys[i].pt.y;
		}

		cv::undistortPoints(
			mat,				//输入的特征点坐标
			mat,				//输出的校正后的特征点坐标
			K,					//相机的内参数矩阵
			Distort,			//相机畸变参数矩阵
			cv::Mat(),			//一个空矩阵，对应为函数原型中的R
			K); 				//新内参数矩阵，对应为函数原型中的P

		//调整回只有一个通道，回归我们正常的处理方式
		mat = mat.reshape(1);

		// Step3 存储校正后的特征点
		mvKeys.resize(N);
		//遍历每一个特征点
		for (int i = 0; i < N; i++)
		{
			//根据索引获取这个特征点
			//注意之所以这样做而不是直接重新声明一个特征点对象的目的是，能够得到源特征点对象的其他属性
			cv::KeyPoint kp = mvKeys[i];
			//读取校正后的坐标并覆盖老坐标
			kp.pt.x = mat.at<float>(i, 0);
			kp.pt.y = mat.at<float>(i, 1);
			mvKeys[i] = kp;
		}
	}

	/**
	 * @brief 计算去畸变图像的边界
	 *
	 * @param[in] imLeft            需要计算边界的图像
	 */
	void Frame::ComputeImageBounds(const cv::Mat &imLeft, const cv::Mat &K, const cv::Mat& Distort)
	{
		// 如果畸变参数不为0，用OpenCV函数进行畸变矫正
		if (Distort.at<float>(0) != 0.0)
		{
			// 保存矫正前的图像四个边界点坐标： (0,0) (cols,0) (0,rows) (cols,rows)
			cv::Mat mat(4, 2, CV_32F);
			mat.at<float>(0, 0) = 0.0;         //左上
			mat.at<float>(0, 1) = 0.0;
			mat.at<float>(1, 0) = imLeft.cols; //右上
			mat.at<float>(1, 1) = 0.0;
			mat.at<float>(2, 0) = 0.0;         //左下
			mat.at<float>(2, 1) = imLeft.rows;
			mat.at<float>(3, 0) = imLeft.cols; //右下
			mat.at<float>(3, 1) = imLeft.rows;

			// 和前面校正特征点一样的操作，将这几个边界点作为输入进行校正
			mat = mat.reshape(2);
			cv::undistortPoints(mat, mat, K, Distort, cv::Mat(), K);
			mat = mat.reshape(1);

			//校正后的四个边界点已经不能够围成一个严格的矩形，因此在这个四边形的外侧加边框作为坐标的边界
			mnMinX = std::min(mat.at<float>(0, 0), mat.at<float>(2, 0));//左上和左下横坐标最小的
			mnMaxX = std::max(mat.at<float>(1, 0), mat.at<float>(3, 0));//右上和右下横坐标最大的
			mnMinY = std::min(mat.at<float>(0, 1), mat.at<float>(1, 1));//左上和右上纵坐标最小的
			mnMaxY = std::max(mat.at<float>(2, 1), mat.at<float>(3, 1));//左下和右下纵坐标最小的
		}
		else
		{
			// 如果畸变参数为0，就直接获得图像边界
			mnMinX = 0.0f;
			mnMaxX = imLeft.cols;
			mnMinY = 0.0f;
			mnMaxY = imLeft.rows;
		}
	}

	/**
	 * @brief 计算某个特征点所在网格的网格坐标，如果找到特征点所在的网格坐标，记录在nGridPosX,nGridPosY里，返回true，没找到返回false
	 *
	 * @param[in] kp                    给定的特征点
	 * @param[in & out] posX            特征点所在网格坐标的横坐标
	 * @param[in & out] posY            特征点所在网格坐标的纵坐标
	 * @return true                     如果找到特征点所在的网格坐标，返回true
	 * @return false                    没找到返回false
	 */
	bool Frame::PosInGrid(const cv::KeyPoint &kp, int &posX, int &posY)
	{
		// 计算特征点x,y坐标落在哪个网格内，网格坐标为posX，posY
		// mfGridElementWidthInv=(FRAME_GRID_COLS)/(mnMaxX-mnMinX);
		// mfGridElementHeightInv=(FRAME_GRID_ROWS)/(mnMaxY-mnMinY);
		posX = std::floor((kp.pt.x - mnMinX)*mfGridElementWidthInv);
		posY = std::floor((kp.pt.y - mnMinY)*mfGridElementHeightInv);

		// 因为特征点进行了去畸变，而且前面计算是round取整，所以有可能得到的点落在图像网格坐标外面
		// 如果网格坐标posX，posY超出了[0,FRAME_GRID_COLS] 和[0,FRAME_GRID_ROWS]，表示该特征点没有对应网格坐标，返回false
		if (posX < 0 || posX >= FRAME_GRID_COLS || posY < 0 || posY >= FRAME_GRID_ROWS)
			return false;

		// 计算成功返回true
		return true;
	}

	/**
	 * @brief 判断地图点是否在视野中
	 * 步骤
	 * Step 1 获得这个地图点的世界坐标，经过以下层层关卡的判断，通过的地图点才认为是在视野中
	 * Step 2 关卡一：将这个地图点变换到当前帧的相机坐标系下，如果深度值为正才能继续下一步。
	 * Step 3 关卡二：将地图点投影到当前帧的像素坐标，如果在图像有效范围内才能继续下一步。
	 * Step 4 关卡三：计算地图点到相机中心的距离，如果在有效距离范围内才能继续下一步。
	 * Step 5 关卡四：计算当前相机指向地图点向量和地图点的平均观测方向夹角，小于60°才能进入下一步。
	 * Step 6 根据地图点到光心的距离来预测一个尺度（仿照特征点金字塔层级）
	 * Step 7 记录计算得到的一些参数
	 * @param[in] pMP                       当前地图点
	 * @param[in] viewingCosLimit           当前相机指向地图点向量和地图点的平均观测方向夹角余弦阈值
	 * @return true                         地图点合格，且在视野内
	 * @return false                        地图点不合格，抛弃
	 */
	bool Frame::isInFrustum(MapPoint *pMP, float viewingCosLimit)
	{
		// 这个标志的确定要经过多个函数的确定，isInFrustum()只是其中的一个验证关卡。这里默认设置为否
		pMP->mbTrackInView = false;

		// Step 1 获得这个地图点的世界坐标
		cv::Mat P = pMP->GetWorldPos();

		// 根据当前帧(粗糙)位姿转化到当前相机坐标系下的三维点Pc
		const cv::Mat Pc = mRcw * P + mtcw;
		const float &PcX = Pc.at<float>(0);
		const float &PcY = Pc.at<float>(1);
		const float &PcZ = Pc.at<float>(2);

		// Step 2 关卡一：将这个地图点变换到当前帧的相机坐标系下，如果深度值为正才能继续下一步。
		if (PcZ < 0.0f)
			return false;

		// Step 3 关卡二：将地图点投影到当前帧的像素坐标，如果在图像有效范围内才能继续下一步。
		const float invz = 1.0f / PcZ;
		const float u = mK.at<double>(0, 0) * PcX*invz + mK.at<double>(0, 2);
		const float v = mK.at<double>(1, 1) * PcY*invz + mK.at<double>(1, 2);

		// 判断是否在图像边界中，只要不在那么就说明无法在当前帧下进行重投影
		if (u<mnMinX || u>mnMaxX)
			return false;
		if (v<mnMinY || v>mnMaxY)
			return false;

		// Step 4 关卡三：计算地图点到相机中心的距离，如果在有效距离范围内才能继续下一步。
		// 得到认为的可靠距离范围:[0.8f*mfMinDistance, 1.2f*mfMaxDistance]
		const float maxDistance = pMP->GetMaxDistanceInvariance();
		const float minDistance = pMP->GetMinDistanceInvariance();

		// 得到当前地图点距离当前帧相机光心的距离,注意P，mOw都是在同一坐标系下才可以
		// mOw：当前相机光心在世界坐标系下坐标
		const cv::Mat PO = P - mOw;
		//取模就得到了距离
		const float dist = cv::norm(PO);

		//如果不在有效范围内，认为投影不可靠
		if (dist<minDistance || dist>maxDistance)
			return false;

		// Step 5 关卡四：计算当前相机指向地图点向量和地图点的平均观测方向夹角，小于60°才能进入下一步。
		cv::Mat Pn = pMP->GetNormal();

		// 计算当前相机指向地图点向量和地图点的平均观测方向夹角的余弦值，注意平均观测方向为单位向量
		const float viewCos = PO.dot(Pn) / dist;

		//夹角要在60°范围内，否则认为观测方向太偏了，重投影不可靠，返回false
		if (viewCos < viewingCosLimit)
			return false;

		// Step 6 根据地图点到光心的距离来预测一个尺度（仿照特征点金字塔层级）
		const int nPredictedLevel = pMP->PredictScale(dist, this);	

		// Step 7 记录计算得到的一些参数
		// 通过置位标记 MapPoint::mbTrackInView 来表示这个地图点要被投影 
		pMP->mbTrackInView = true;

		// 该地图点投影在当前图像（一般是左图）的像素横坐标
		pMP->mTrackProjX = u;

		// 该地图点投影在当前图像（一般是左图）的像素纵坐标									
		pMP->mTrackProjY = v;

		// 根据地图点到光心距离，预测的该地图点的尺度层级
		pMP->mnTrackScaleLevel = nPredictedLevel;

		// 保存当前相机指向地图点向量和地图点的平均观测方向夹角的余弦值
		pMP->mTrackViewCos = viewCos;

		//执行到这里说明这个地图点在相机的视野中并且进行重投影是可靠的，返回true
		return true;
	}
}