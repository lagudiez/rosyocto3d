#ifndef YOCTO3D_H
#define YOCTO3d_H


namespace yocto3d
{
	void initialize();
	struct Vector3{
		float x;
		float y;
		float z;
		Vector3(float x, float y, float z){
			this->x = x;
			this->y = y;
			this->z = z;
		}
	};
	bool isConnected();
	Vector3 getAcceleration();
}

#endif // YOCTO3D_H
